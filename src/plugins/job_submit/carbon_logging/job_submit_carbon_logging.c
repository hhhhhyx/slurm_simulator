/*****************************************************************************\
 *  job_submit_partition.c - Set default partition in job submit request
 *  specifications.
 *****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <regex.h>
#include <curl/curl.h>
#include <time.h>

#include "config.h"

#if HAVE_JSON_C_INC
#include <json-c/json.h>
#else
#include <json/json.h>
#endif


#include "slurm/slurm_errno.h"
#include "src/common/slurm_xlator.h"

#include "src/common/xstring.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"

#define KWH 2.77778e-7

#ifndef LCA_FACTOR
#define LCA_FACTOR 6.342*1e-9 //1 / lca in seconds
#endif


#ifndef POWER_CPU
#define POWER_CPU 165. //W
#endif

#ifndef POWER_GPU
#define POWER_GPU 300. //W
#endif

#define MEM_UNIT 8000 //8GB ->8000MB
#ifndef POWER_MEM
#define POWER_MEM 1. // W per 8GB
#endif

#ifndef EM_CPU
#define EM_CPU 10. //Intel Xeon Gold 6240R (14nm, 24 cores) kgCO2
#endif

#ifndef EM_GPU
#define EM_GPU 20. // NVIDIA V100 kgCO2
#endif

#ifndef EM_MEM
#define EM_MEM 1. // 8GB module
#endif

/* Required to get the carbon intensity */

#define TIME_FORMAT "%Y-%m-%dT%H:%M:%SZ"
#define MAX_RECORDS 48 // 24h, 30min per record. It should be no more than 48
#ifndef REGION_ID
#define REGION_ID 16 // Scotland
#endif
#define MAX_SCI_VALUE 100 //


/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *	<application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "auth" for Slurm authentication) and <method> is a
 * description of how this plugin satisfies that application.  Slurm will
 * only load authentication plugins if the plugin_type string has a prefix
 * of "auth/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]       	= "Job submit carbon logging plugin";
const char plugin_type[]       	= "job_submit/carbon_logging";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/*****************************************************************************\
 * We've provided a simple example of the type of things you can do with this
 * plugin. If you develop another plugin that may be of interest to others
 * please post it to slurm-dev@schedmd.com  Thanks!
\*****************************************************************************/



/* Parses tres_per_node and returns the requested GPUs per node */
extern int regex_gpus(char *tres_per_node)
{
	const char *pattern = "gres:gpu:([0-9]+)";
	regex_t regex;
	regmatch_t pmatch[2];
	char match[10];
	int num_gpus = 0;

	if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
		error("Could not compile regex");
		return 0;
	}

	if (regexec(&regex, tres_per_node, 2, pmatch, 0) == 0) {
		// Extract the matched substring for GPU count
		int len = pmatch[1].rm_eo - pmatch[1].rm_so;
		strncpy(match, tres_per_node + pmatch[1].rm_so, len);
		match[len] = '\0';
		num_gpus = atoi(match);
	} else {
		error("No match found");
	}

	regfree(&regex);

	return num_gpus;
}

/* Returns current time in ISO8601 format in from */
void get_iso8601_time(char *from, size_t buffer_size) {
	time_t now = time(NULL);
	struct tm *tm_info = gmtime(&now);
	strftime(from, buffer_size, TIME_FORMAT, tm_info);
}

/* Callback to handle the HTTP response */
static size_t _write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    char **buffer = (char**)userp;
	if (*buffer == NULL) {
		*buffer = (char*)malloc(1);
	}

    // Allocate a new buffer with the desired size
    char *new_buffer = malloc(strlen(*buffer) + realsize + 1);
    if (new_buffer == NULL) {
        fprintf(stderr, "out of memory\n");
        return 0;
    }

    // Copy the old buffer contents to the new buffer
    strcpy(new_buffer, *buffer);

    // Copy the new data to the end of the new buffer
    memcpy(&new_buffer[strlen(*buffer)], contents, realsize);
    new_buffer[strlen(*buffer) + realsize] = '\0';

    // Free the old buffer
	if (**buffer)
	    free(*buffer);

    // Update the buffer pointer
    *buffer = new_buffer;

    return realsize;
}

uint32_t _get_intensity(char *url) {
 CURL *curl;
    CURLcode res;
	char * response="";
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        res = curl_easy_perform(curl);
		fprintf(stderr, "%s\n", url);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
	}
	json_object *root_obj = json_tokener_parse(response);
    if (!root_obj) {
        fprintf(stderr, "Error parsing JSON: %s\n", response);
        json_object_put(root_obj);
        curl_easy_cleanup(curl);
        free(response);
		return -1;

    }
	json_object *first_region;
    json_object *data_array = json_object_object_get(root_obj, "data");
    if (!data_array || !json_object_is_type(data_array, json_type_array)) {
        //fprintf(stderr, "Error: 'data' is not an array\n");
		if (!data_array) {
        	json_object_put(root_obj);
        	curl_easy_cleanup(curl);
        	free(response);
			return -1;
		}
		first_region = data_array;
    }
	else {

    first_region = json_object_array_get_idx(data_array, 0);
    if (!first_region || !json_object_is_type(first_region, json_type_object)) {
        fprintf(stderr, "Error: First region is not an object\n");
        json_object_put(root_obj);
        curl_easy_cleanup(curl);
        free(response);
		return -1;

            }
	}
    json_object *region_data_array = json_object_object_get(first_region, "data");
    if (!region_data_array || !json_object_is_type(region_data_array, json_type_array)) {
        fprintf(stderr, "Error: 'data' in region is not an array\n");
//        json_object_put(root_obj);
//        curl_easy_cleanup(curl);
//        free(response);
//		return -1;

    }
	//Forecast:last item else first item
	int array_length = json_object_array_length(region_data_array);
	json_object *first_data_item = json_object_array_get_idx(region_data_array, array_length -1);
	
    //json_object *first_data_item = json_object_array_get_idx(region_data_array, 0);
    if (!first_data_item || !json_object_is_type(first_data_item, json_type_object)) {
        fprintf(stderr, "Error: First data item is not an object\n");
        json_object_put(root_obj);
        curl_easy_cleanup(curl);
        free(response);
		return -1;

    }

    json_object *intensity_obj = json_object_object_get(first_data_item, "intensity");
    if (!intensity_obj || !json_object_is_type(intensity_obj, json_type_object)) {
        fprintf(stderr, "Error: 'intensity' is not an object\n");
        json_object_put(root_obj);
        curl_easy_cleanup(curl);
        free(response);
		return -1;

    }

    json_object *forecast_value = json_object_object_get(intensity_obj, "forecast");
    if (!forecast_value || !json_object_is_type(forecast_value, json_type_int)) {
        fprintf(stderr, "Error: 'forecast' is not an integer\n");
        json_object_put(root_obj);
        curl_easy_cleanup(curl);
        free(response);
		return -1;
    }

    uint32_t forecast = (uint32_t)json_object_get_int(forecast_value);
	const char * str_forecast = json_object_get_string(forecast_value);
	fprintf(stderr, "Forecast: as str %s as int %u\n", str_forecast, forecast);

        json_object_put(root_obj);
        curl_easy_cleanup(curl);
        free(response);
	return forecast;		
}


uint32_t _get_intensity_now() {
	// https://carbon-intensity.github.io/api-definitions/#get-regional-regionid-regionid
	char from[25];
	char url[256];

	get_iso8601_time(from, sizeof(from));
	snprintf(url, sizeof(url),
	 		"https://api.carbonintensity.org.uk/regional/regionid/%d",
	 		 REGION_ID);
	return _get_intensity(url);
}

uint32_t _get_intensity_forecast() {
// doc: https://carbon-intensity.github.io/api-definitions/#get-regional-intensity-from-fw24h
	char from[25];
	char url[256];

	get_iso8601_time(from, sizeof(from));
	snprintf(url, sizeof(url),
	 		"https://api.carbonintensity.org.uk/regional/intensity/%s/fw24h/regionid/%d",
	 		from, REGION_ID);
	return _get_intensity (url);
}


void _compute_carbon(job_desc_msg_t *job_desc, part_record_t * part_ptr) 
{
	
	uint32_t current_intensity = _get_intensity_now();
	uint32_t future_intensity = _get_intensity_forecast();
	fprintf(stderr, "current_intensity %u future_intensity %u\n", current_intensity, future_intensity);

	// SCI = (operational carbon + embodied carbon) / walltime
	uint32_t walltime =job_desc->time_limit * 60; // in secs

	// Energy = Number of Nodes * Power of the node * Walltime
	uint32_t num_nodes = job_desc->num_tasks / job_desc->ntasks_per_node;

	uint16_t cpus_per_task = (job_desc->cpus_per_task == UINT16_MAX - 1 ) ? 1: job_desc->cpus_per_task;
	uint32_t total_cpus = part_ptr->max_core_cnt; // could also be max_cpu_cnt;
	//uint32_t total_memory = part_ptr->max_mem_per_cpu;
	// Power of the node = Power of the CPU + Power of the GPUs + Power of DRAM + Power of any SSDs/HDDs
	
	float power_cpus = ((cpus_per_task * job_desc->ntasks_per_node) / (float)total_cpus) * POWER_CPU; // W

	double mem_per_node; // in units of 8GB
	//Work with memory in MB to avoid overflows
	if (job_desc->pn_min_memory == UINT64_MAX -1) {
		if (part_ptr->max_mem_per_cpu != 0) {
			mem_per_node = part_ptr->max_mem_per_cpu * 1e-6 /  MEM_UNIT; //mem in MB, divide by unit
		}
		else
			mem_per_node=2*((cpus_per_task * job_desc->ntasks_per_node)/ (double)total_cpus);	//assign 16G of memory per node, and share
	}
	else {
		mem_per_node = (double)job_desc->pn_min_memory * 1e-6 / MEM_UNIT;//mem in MB, divide by unit
	}

	//uint64_t mem_per_node = (job_desc->pn_min_memory == UINT64_MAX - 1) ? (part_ptr->max_mem_per_cpu != 0 ? part_ptr->max_mem_per_cpu : (uint64_t)(MEM_UNIT)) : job_desc->pn_min_memory;
	double power_mem = (double)mem_per_node*(double)POWER_MEM;
	float power_gpus = 0;
	int gpus_per_node = 0;
	if (job_desc->tres_per_node != NULL) {
		gpus_per_node = regex_gpus(job_desc->tres_per_node);
		power_gpus = POWER_GPU * gpus_per_node;
	}

	double power_nodes = power_cpus + power_gpus + power_mem;
	fprintf(stderr,"min_mem %lu max_mem %lu mem_per_node %.3lf power_mem %.3f power_gpus %.3f\n", job_desc->pn_min_memory, part_ptr->max_mem_per_cpu, mem_per_node,power_mem, power_gpus);
	double energy = num_nodes * power_nodes * walltime; // J = W * sec

	// Opertional Emissions = Energy * Region-specific carbon intensity
	double op_emissions = (energy * KWH) * current_intensity; // kWH * gCO2/kWh -> gCO2
	double op_emissions_forecast = (energy * KWH) *future_intensity;
	fprintf(stderr, "Operational %.3lf %.3lf\n", op_emissions, op_emissions_forecast);

	// Embodied Emissions = Total embodied emissions * Time share * Resource share
	double time_share = (double)(walltime/60.) * LCA_FACTOR; // mins/mins
	double em_cpu =  (((cpus_per_task * job_desc->ntasks_per_node) / (float)total_cpus))*EM_CPU ;
	double em_gpu = job_desc->tres_per_node != NULL ? gpus_per_node * EM_GPU : 0;
	double em_mem = mem_per_node * EM_MEM;
	double em_emissions = (em_cpu + em_gpu + em_mem) * num_nodes * time_share; // gCO2
	fprintf(stderr, "em_cpu %.4lf em_gpu %.4lf em_mem %.4lf time_share %.4e emissions %.4lf\n",em_cpu, em_gpu, em_mem, time_share, em_emissions);

	job_desc->power_est = num_nodes * power_nodes;
	job_desc->energy_est = energy;
	job_desc->sci_est = op_emissions + em_emissions;
	job_desc->emissions_start = 1016;
	fprintf(stderr,"emissions_start job_submit_carbon_logging %d\n",job_desc->emissions_start);
	job_desc->sci_fcst = op_emissions_forecast + em_emissions;
	double change_rate = (future_intensity - current_intensity) / current_intensity;

	if (change_rate > 0) {
		job_desc->change_rate = change_rate + job_desc->sci_est / MAX_SCI_VALUE;
	} else {
		job_desc->change_rate = change_rate - job_desc->sci_est / MAX_SCI_VALUE;
	}
	job_desc->change_rate = (job_desc->sci_fcst - job_desc->sci_est) / job_desc->sci_est;



}


/* Test if this user can run jobs in the selected partition based upon
 * the partition's AllowGroups parameter. */
static bool _user_access(uid_t run_uid, uint32_t submit_uid,
			 part_record_t *part_ptr)
{
	int i;

	if (run_uid == 0) {
		if (part_ptr->flags & PART_FLAG_NO_ROOT)
			return false;
		return true;
	}

	if ((part_ptr->flags & PART_FLAG_ROOT_ONLY) && (submit_uid != 0))
		return false;

	if (part_ptr->allow_uids == NULL)
		return true;	/* AllowGroups=ALL */

	for (i=0; part_ptr->allow_uids[i]; i++) {
		if (part_ptr->allow_uids[i] == run_uid)
			return true;	/* User in AllowGroups */
	}
	return false;		/* User not in AllowGroups */
}

static bool _valid_memory(part_record_t *part_ptr, job_desc_msg_t *job_desc)
{
	uint64_t job_limit, part_limit;

	if (!part_ptr->max_mem_per_cpu)
		return true;

	if (job_desc->pn_min_memory == NO_VAL64)
		return true;

	if ((job_desc->pn_min_memory   & MEM_PER_CPU) &&
	    (part_ptr->max_mem_per_cpu & MEM_PER_CPU)) {
		/* Perform per CPU memory limit test */
		job_limit  = job_desc->pn_min_memory   & (~MEM_PER_CPU);
		part_limit = part_ptr->max_mem_per_cpu & (~MEM_PER_CPU);
		if (job_desc->pn_min_cpus != NO_VAL16) {
			job_limit  *= job_desc->pn_min_cpus;
			part_limit *= job_desc->pn_min_cpus;
		}
	} else if (((job_desc->pn_min_memory   & MEM_PER_CPU) == 0) &&
		   ((part_ptr->max_mem_per_cpu & MEM_PER_CPU) == 0)) {
		/* Perform per node memory limit test */
		job_limit  = job_desc->pn_min_memory;
		part_limit = part_ptr->max_mem_per_cpu;
	} else {
		/* Can not compare per node to per CPU memory limits */
		return true;
	}

	if (job_limit > part_limit) {
		debug("job_submit/partition: skipping partition %s due to "
		      "memory limit (%"PRIu64" > %"PRIu64")",
		      part_ptr->name, job_limit, part_limit);
		return false;
	}

	return true;
}


/* This example code will set a job's default partition to the partition with
 * highest priority_tier is available to this user. This is only an example
 * and tremendous flexibility is available. */
extern int job_submit(job_desc_msg_t *job_desc, uint32_t submit_uid,
		      char **err_msg)
{
	ListIterator part_iterator;
	part_record_t *part_ptr;
	part_record_t *top_prio_part = NULL;
	part_record_t *this;
	part_iterator = list_iterator_create(part_list);

 	if (!job_desc->partition) { /* job doesn't specify partition */
		while ((part_ptr = list_next(part_iterator))) {
			if (!(part_ptr->state_up & PARTITION_SUBMIT))
				continue;	/* nobody can submit jobs here */
			if (!_user_access(job_desc->user_id, submit_uid, part_ptr))
				continue;	/* AllowGroups prevents use */

			if (!top_prio_part ||
		    	(top_prio_part->priority_tier < part_ptr->priority_tier)) {
				/* Test job specification elements here */
				if (!_valid_memory(part_ptr, job_desc))
					continue;

				/* Found higher priority partition */
				top_prio_part = part_ptr;
			}
		}

		if (top_prio_part) {
			this = part_ptr;
			info("Setting partition of submitted job to %s",
			     top_prio_part->name);
			job_desc->partition = xstrdup(top_prio_part->name);
		}
	}
	else {
		while ((part_ptr = list_next(part_iterator))) {	
			if (strcmp(part_ptr->name, job_desc->partition) == 0) {
				this = part_ptr;
				break;
			}
		}

	}
	list_iterator_destroy(part_iterator);


	/* Log select fields from a job submit request. See slurm/slurm.h
	 * for information about additional fields in job_desc_msg_t.
	 * Note that default values for most numbers is NO_VAL */
	_compute_carbon(job_desc, this); 

	info("Job submit request: account:%s begin_time:%ld dependency:%s "
	     "name:%s partition:%s qos:%s submit_uid:%u time_limit:%u "
	     "user_id:%u power_est:%.3f energy_est:%.3f sci_est:%.3f "
		 "sci_fcst:%.3f change_rate:%.3f",
	     job_desc->account, (long)job_desc->begin_time,
	     job_desc->dependency,
	     job_desc->name, job_desc->partition, job_desc->qos,
	     submit_uid, job_desc->time_limit, job_desc->user_id, 
		 job_desc->power_est, job_desc->energy_est, job_desc->sci_est,
		 job_desc->sci_fcst, job_desc->change_rate);


	return SLURM_SUCCESS;
}

extern int job_modify(job_desc_msg_t *job_desc, job_record_t *job_ptr,
		      uint32_t submit_uid, char **err_msg)
{
	return SLURM_SUCCESS;
}


