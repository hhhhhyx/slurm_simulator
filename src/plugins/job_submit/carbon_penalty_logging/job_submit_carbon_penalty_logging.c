/*****************************************************************************\
 *  job_submit_logging.c - Log job submit request specifications.
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
#include <curl/curl.h>
#include <json-c/json.h>
#include <string.h>
#include <math.h>
#include <regex.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/slurm_xlator.h"
#include "src/slurmctld/slurmctld.h"

#define REGION_ID 16 // Scotland

#define MAX_DATA_SIZE 48
#define MEM_UNIT 8000 //8GB ->8000MB
#define KWH 2.77778e-7

#ifndef POWER_CPU
#define POWER_CPU 165. //W
#endif

#ifndef POWER_GPU
#define POWER_GPU 300. //W
#endif

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

#ifndef LCA_FACTOR
#define LCA_FACTOR 6.342*1e-9 //1 / lca in seconds
#endif


#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define LOG_DIR "/home/slurm/work/micro_cluster/jobs"
#define MAX_FILENAME 256
#define MAX_LINE 1024
#define TIME_FORMAT "%Y-%m-%dT%H:%M:%SZ"


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
const char plugin_name[]       	= "Job submit carbon penalty logging plugin";
const char plugin_type[]       	= "job_submit/carbon_penalty_logging";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/*****************************************************************************\
 * We've provided a simple example of the type of things you can do with this
 * plugin. If you develop another plugin that may be of interest to others
 * please post it to slurm-dev@schedmd.com  Thanks!
\*****************************************************************************/

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


void get_iso8601_time(char *from, size_t buffer_size) {
	time_t now = time(NULL);
	struct tm *tm_info = gmtime(&now);
	strftime(from, buffer_size, TIME_FORMAT, tm_info);
}

ci_data_array dispose_carbon_intensity_api_info(char *url) {

   ci_data_array result;

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
      return result;
    }
    //printf("%s\n", json_object_to_json_string(root_obj));

   json_object *first_layer_data;
   json_object *data_obj = json_object_object_get(root_obj, "data");
   if (!data_obj || !json_object_is_type(data_obj, json_type_array)) { //if the first layer data is not an array
      //fprintf(stderr, "Error: 'data' is not an array\n");
      if (!data_obj) {
         json_object_put(root_obj);
         curl_easy_cleanup(curl);
         free(response);
         return result;
      }
      first_layer_data = data_obj;
      } else { // if the first layer data is an array
         first_layer_data = json_object_array_get_idx(data_obj, 0);
         if (!first_layer_data || !json_object_is_type(first_layer_data, json_type_object)) {
            fprintf(stderr, "Error: First region is not an object\n");
            json_object_put(root_obj);
            curl_easy_cleanup(curl);
            free(response);
            return result;
         }
   }
   //printf("%s\n", json_object_to_json_string(data_obj));
       /* dispose the seconde data layer, convert into a array list*/
   json_object *layer_data_array = json_object_object_get(first_layer_data, "data");
   if (!layer_data_array || !json_object_is_type(layer_data_array, json_type_array)) {
      fprintf(stderr, "Error: 'data' in region is not an array\n");
   }
   int array_length = json_object_array_length(layer_data_array);// to get the array list size
   //printf("%d",array_length);

   //printf("%s\n", json_object_to_json_string(layer_data_array));
    result.capacity = array_length;
   // result.items = NULL;
    result.size = 0;

   for(int i=0; i< array_length;i++) {

      json_object *ci_data_item = json_object_array_get_idx(layer_data_array, i);
      //printf("%s\n",json_object_to_json_string(ci_data_item));
      if (!ci_data_item || !json_object_is_type(ci_data_item, json_type_object)) {
         fprintf(stderr, "Error: First data item is not an object\n");
         json_object_put(root_obj);
         curl_easy_cleanup(curl);
         free(response);
         return result;
      }

      json_object *from_obj = json_object_object_get(ci_data_item, "from");
      //printf("%s\n",json_object_to_json_string(from_obj));
      if (!from_obj || !json_object_is_type(from_obj, json_type_string)) {
         fprintf(stderr, "Error: 'from' is not an string\n");
         json_object_put(root_obj);
         curl_easy_cleanup(curl);
         free(response);
         return result;
      }
      
      json_object *to_obj = json_object_object_get(ci_data_item, "to");
      //printf("%s\n",json_object_to_json_string(to_obj));
      if (!to_obj || !json_object_is_type(to_obj, json_type_string)) {
         fprintf(stderr, "Error: 'to' is not an string\n");
         json_object_put(root_obj);
         curl_easy_cleanup(curl);
         free(response);
         return result;
      }

      json_object *intensity_obj = json_object_object_get(ci_data_item, "intensity");

      if (!intensity_obj || !json_object_is_type(intensity_obj, json_type_object)) {
         fprintf(stderr, "Error: 'intensity' is not an object\n");
         json_object_put(root_obj);
         curl_easy_cleanup(curl);
         free(response);
         return result;
      }

      json_object *forecast_value = json_object_object_get(intensity_obj, "forecast"); // the carbon intensity 
      //printf("%d\n",forecast_value);
      if (!forecast_value || !json_object_is_type(forecast_value, json_type_int)) {
         fprintf(stderr, "Error: 'forecast' is not an string\n");
         json_object_put(root_obj);
         curl_easy_cleanup(curl);
         free(response);
         return result;
      }

      // result.items[i].from = strdup(json_object_get_string(from_obj));
      // result.items[i].to = strdup(json_object_get_string(to_obj));
      result.items[i].intensity = json_object_get_int(forecast_value);
      result.items[i].index = i;
      result.size += 1;
   }

   return result;

}


/* get the standard deviation of carbon intensity */
/* get the past 24h carbon intensity info*/
ci_data_array get_past_24h_carbon_intensity_info() {
   char from[25];
	char url[256];

	get_iso8601_time(from, sizeof(from));
    snprintf(url, sizeof(url),
             "https://api.carbonintensity.org.uk/regional/intensity/%s/pt24h/regionid/%d",
            from, REGION_ID);

    return dispose_carbon_intensity_api_info(url);
}

ci_data_array get_next_24h_carbon_intensity_info() {
   char from[25];
	char url[256];

	get_iso8601_time(from, sizeof(from));
    snprintf(url, sizeof(url),
             "https://api.carbonintensity.org.uk/regional/intensity/%s/fw24h/regionid/%d",
            from, REGION_ID);

    return dispose_carbon_intensity_api_info(url);
}

ci_data_array get_next_1h_carbon_intensity_info() {
   ci_data_array next_24h = get_next_24h_carbon_intensity_info();

   ci_data_array next_1h;
   next_1h.capacity = 2;
   next_1h.size = 2;

   next_1h.items[0] = next_24h.items[0];
   next_1h.items[1] = next_24h.items[1];

   return next_1h;
}

ci_data_array get_currentt_carbon_intensity_info(){
   char from[25];
	char url[256];

	get_iso8601_time(from, sizeof(from));
	snprintf(url, sizeof(url),
	 		"https://api.carbonintensity.org.uk/regional/regionid/%d",
	 		 REGION_ID);
	return dispose_carbon_intensity_api_info(url);
}



/* get the average value（mean) of carbon intensity */
double get_mean_value_of_carbon_intensity(ci_data_array ci_array) {
    double mean;
    double sum = 0.0;
    /* calculate the average value */
    for(int i =0; i < ci_array.size;i++) {
        sum += ci_array.items[i].intensity;
    }
    mean = (double)(sum/ci_array.size);
    return mean;
}

/* get the standard deviation of carbon intensity */
double get_standard_deviation_of_carbon_intensity(ci_data_array ci_array) {
    double mean = get_mean_value_of_carbon_intensity(ci_array);
    double sum_squared_diff = 0.0;

    for(int i=0; i < ci_array.size; i++) {
        sum_squared_diff += pow(ci_array.items[i].intensity - mean,2);
    }

    double variance = sum_squared_diff/ci_array.size;
    return sqrt(variance);
}

/* get high/middle/low carbon intensity time period */
CI_TIME_PERIOD get_carbon_intensity_time_period(int current_ci) {

   ci_data_array previous_24h_ci = get_past_24h_carbon_intensity_info();

   double mean = get_mean_value_of_carbon_intensity(previous_24h_ci);
   double standard_deviation = get_standard_deviation_of_carbon_intensity(previous_24h_ci);
   double low_ci_threshold = mean - 0.5 * standard_deviation;
   double high_ci_threshold = mean + 0.5 * standard_deviation;

   if (current_ci <= low_ci_threshold)
      return CI_LOW;
   else if (current_ci >= high_ci_threshold)
      return CI_HIGH;
   else 
      return CI_MIDDLE;
}

/* Parses tres_per_node and returns the requested GPUs per node */
extern int regex_gpus(char *tres_per_node)
{
	const char *pattern = "gres:gpu:([0-9]+)";
	regex_t regex;
	regmatch_t pmatch[2];
	char match[10];
	int num_gpus = 0;

	if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
		//error("Could not compile regex");
		return 0;
	}

	if (regexec(&regex, tres_per_node, 2, pmatch, 0) == 0) {
		// Extract the matched substring for GPU count
		int len = pmatch[1].rm_eo - pmatch[1].rm_so;
		strncpy(match, tres_per_node + pmatch[1].rm_so, len);
		match[len] = '\0';
		num_gpus = atoi(match);
	} else {
		//error("No match found");
	}

	regfree(&regex);

	return num_gpus;
}

void calculate_current_carbon_emissions(job_desc_msg_t *job_desc, part_record_t * part_ptr) {

   //get ci_max_1h
   ci_data_array next_24h_ci = get_next_24h_carbon_intensity_info();

   ci_data_array next_1h_ci;
   next_1h_ci.capacity = 2;
   next_1h_ci.size = 2;

   next_1h_ci.items[0] = next_24h_ci.items[0];
   next_1h_ci.items[1] = next_24h_ci.items[1];
   
   int ci_max_1h = MAX(next_1h_ci.items[0].intensity,next_1h_ci.items[1].intensity);
   fprintf(stderr,"ci_max_1h: %u\n", ci_max_1h);
   //get current ci
   // ci_data_array current_ci_info = get_currentt_carbon_intensity_info();
   int current_intensity = next_24h_ci.items[0].intensity;
   fprintf(stderr,"current_tensity: %u\n", current_intensity);


   //calculate op_emissions
   uint32_t walltime =job_desc->time_limit * 60; // in secs
   fprintf(stderr,"walltime: %u\n", walltime);

   // Energy = Number of Nodes * Power of the node * Walltime
   uint32_t num_nodes = job_desc->num_tasks / job_desc->ntasks_per_node;
   fprintf(stderr,"num_nodes: %u\n", num_nodes);

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
   double energy_max = num_nodes * power_nodes * 3600; //in 1h J = W * sec
   double energy   = num_nodes * power_nodes * walltime; // J = W * sec 

   //get operational emissions
   double op_emissions_max = (energy_max * KWH) * ci_max_1h;
   double op_emissions_current = (energy * KWH) * current_intensity;
   fprintf(stderr, "op_emissions_max: %.3lf , op_emissions_current %.3lf\n", op_emissions_max, op_emissions_current);

   //get embodied emissions
   double em_cpu =  (((cpus_per_task * job_desc->ntasks_per_node) / (float)total_cpus))*EM_CPU ;
   double em_gpu = job_desc->tres_per_node != NULL ? gpus_per_node * EM_GPU : 0;
   double em_mem = mem_per_node * EM_MEM;
   // double time_share = (double)(walltime/60.) * LCA_FACTOR; // mins/mins
   // double time_share_max = (double)(3600/60.) * LCA_FACTOR; // 1h = 3600s

   double time_share = (double)(walltime/60.) * LCA_FACTOR; // mins/mins
   double time_share_max = (double)(3600/60.) * LCA_FACTOR; // 1h = 3600s

   double em_emissions = (em_cpu + em_gpu + em_mem) * num_nodes * time_share; // gCO2
   double em_emissions_max = (em_cpu + em_gpu + em_mem) * num_nodes * time_share_max; // gCO2
   fprintf(stderr, "time_share_max %f num_nodes %u\n", time_share_max, num_nodes);
   fprintf(stderr, "em_cpu %.4lf em_gpu %.4lf em_mem %.4lf time_share %.4e emissions %.4lf em_emissions_max %.4lf \n",em_cpu, em_gpu, em_mem, time_share, em_emissions,em_emissions_max);

   double carbon_weight = (op_emissions_current + em_emissions) / (op_emissions_max + em_emissions_max); // the first penalty factor, carbon_weight
  

   //set job_des values
   fprintf(stderr, "em_emissions: %.3lf , op_emissions_current %.3lf\n", em_emissions, op_emissions_current);
   job_desc->emissions_start = op_emissions_current + em_emissions;
   fprintf(stderr, "emissions start %.4lf\n", job_desc->emissions_start);
   job_desc->carbon_weight = carbon_weight;
   fprintf(stderr, "Carbon weight %.4lf\n", carbon_weight);
   job_desc->carbon_intensity_period = get_carbon_intensity_time_period(current_intensity);

   job_desc->em_cpu = em_cpu;
   job_desc->em_gpu = em_gpu;
   job_desc->em_mem = em_mem;
   job_desc->num_nodes = num_nodes;
   job_desc->power_nodes = power_nodes;
   job_desc->em_emissions = em_emissions;

}

/* to get the jobs throughput of last 1hour*/


//to validate the logfile formate
int is_valid_logfile(const char *filename) {
   if(strncmp(filename, "jobcom_",8) != 0) {
      return 0;
   }
   const char *ext = strrchr(filename, ".");
   if(ext == NULL || strcmp(ext, ".log") != 0) {
      return 0;
   }

   return 1;
}

//to get the time of jobcomp.log files
time_t extract_timestamp(const char *filename) {
	// fprintf(stderr,"go into extract timestamp");
    struct tm tm = {0};
    char timestamp[15];
    
    strncpy(timestamp, filename + 8, 14);
    timestamp[14] = '\0';
    
    if (strptime(timestamp, "%Y%m%d_%H%M%S", &tm) == NULL) {
        return -1;
    }
    
    return mktime(&tm);
}

//to count completed jobs in one log
int count_completed_jobs_in_log(const char *filename) {
	// fprintf(stderr, "go into count_completed_jobs_in_log");
	FILE *file = fopen(filename,"r");

	if(file==NULL) {
		perror("File dosen't exist!");
		return 0;
	}

	char line[MAX_LINE];
	int count = 0;

	while(fgets(line, sizeof(line),file)) {
		if(strstr(line, "JobState=COMPLETED") != NULL) {
			count++;
		}
	}

	fclose(file);
	return count;
}

time_t parse_endtime(const char* end_str) {
    struct tm tm;
    memset(&tm, 0, sizeof(struct tm));
    strptime(end_str, "%Y-%m-%dT%H:%M:%S", &tm);
    return mktime(&tm);
}

int is_within_past_hour(time_t job_end) {
   time_t now = time(NULL);
   struct tm *tm_info = gmtime(&now);
   char buffer[25];
   strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", tm_info);
   return difftime(now, job_end) <= 3600 && difftime(now, job_end) >= 0;
}

//to count all completed jobs in all logs of last 1 hour
// int count_total_jobs() {
// 	DIR *dir;
// 	struct dirent *entry;
// 	struct stat file_stat;
// 	time_t now = time(NULL);
// 	time_t one_hour_ago = now - 3600;
// 	int total_complted_jobs = 0;
// 	char path[MAX_FILENAME];

// 	dir = opendir(LOG_DIR);
// 	info("Directory is : %s", LOG_DIR);
// 	if(dir == NULL) {
// 		perror("Directory dosen't exist!");
// 		info("Directory dosen't exist: %s", LOG_DIR);
// 		return EXIT_FAILURE;
// 	}
	
// 	while ((entry = readdir(dir))!=NULL) {
// 		if (!is_valid_logfile(entry->d_name)) {
// 		}

// 		snprintf(path,sizeof(path), "%s/%s", LOG_DIR, entry->d_name);
// 		//fprintf(stderr,"path: %s\n", path);
// 		if (stat(path, &file_stat) != 0) {
// 			perror("Error file stats");
// 			continue;
// 		}
		
// 		time_t file_time = extract_timestamp(entry->d_name);
// 		if (file_time == -1) {
// 			continue;
// 		}

// 		//fprintf(stderr,"target filename: %s\n", path);
// 		// add 1 hour ago jobs count
// 		if(file_time >= one_hour_ago && file_time <=now) {
// 			int count = count_completed_jobs_in_log(path);
// 			//fprintf(stderr, "%s : %u completed jobs\n", entry->d_name,count);
// 			total_complted_jobs += count;
// 		}
// 	}

// 	closedir(dir);
// 	return total_complted_jobs;
// }

int count_total_jobs() {
    DIR *dir;
    struct dirent *entry;
    int total_jobs = 0;

    dir = opendir(LOG_DIR);
    if (!dir) {
        perror("Failed to open job directory");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG || !strstr(entry->d_name, ".log")) continue;

        char filepath[MAX_FILENAME];
        snprintf(filepath, sizeof(filepath), "%s/%s", LOG_DIR, entry->d_name);

        FILE *fp = fopen(filepath, "r");
        if (!fp) {
            perror("Failed to open log file");
            continue;
        }

        char line[MAX_LINE];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "EndTime=") && strstr(line, "JobState=COMPLETED")) {
                char *end_ptr = strstr(line, "EndTime=");
                if (end_ptr) {
                    char end_str[32];
                    sscanf(end_ptr + 8, "%31s", end_str);
                    char *newline = strchr(end_str, '\n');
                    if (newline) *newline = '\0';

                    time_t end_time = parse_endtime(end_str);
                    if (is_within_past_hour(end_time)) {
                        total_jobs++;
                    }
                }
            }
        }

        fclose(fp);
    }

    closedir(dir);
    return total_jobs;
}


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

   info("Invoking job submit carbon penalty logging plugins!");

   calculate_current_carbon_emissions(job_desc,this);
   int one_hour_jobs_throughput = count_total_jobs();
   job_desc->one_hour_ago_job_throughput = one_hour_jobs_throughput;


	/* Log select fields from a job submit request. See slurm/slurm.h
	 * for information about additional fields in job_desc_msg_t.
	 * Note that default values for most numbers is NO_VAL */
	info("Job submit request: account:%s begin_time:%ld dependency:%s "
	     "name:%s partition:%s qos:%s submit_uid:%u time_limit:%u "
	     "user_id:%u "
        "total jobs throughout:%u "
        "carbon weight:%f ",
      job_desc->account, (long)job_desc->begin_time,
      job_desc->dependency,
      job_desc->name, job_desc->partition, job_desc->qos,
      submit_uid, job_desc->time_limit, job_desc->user_id,
      job_desc->one_hour_ago_job_throughput, job_desc->carbon_weight);
   info("Job submit emissions for job %s is %f", job_desc->name, job_desc->emissions_start);

	return SLURM_SUCCESS;
}

extern int job_modify(job_desc_msg_t *job_desc, job_record_t *job_ptr,
		      uint32_t submit_uid, char **err_msg)
{
	/* Log select fields from a job modify request. See slurm/slurm.h
	 * for information about additional fields in job_desc_msg_t.
	 * Note that default values for most numbers is NO_VAL */
   nfo("Job modify request: account:%s begin_time:%ld dependency:%s "
	     "job_id:%u name:%s partition:%s qos:%s submit_uid:%u "
	     "time_limit:%u",
      job_desc->account, (long)job_desc->begin_time,
      job_desc->dependency,
      job_desc->job_id, job_desc->name, job_desc->partition,
      job_desc->qos, submit_uid, job_desc->time_limit);

	return SLURM_SUCCESS;
}

int main() {
    printf("hello world");
    return 0;
}
