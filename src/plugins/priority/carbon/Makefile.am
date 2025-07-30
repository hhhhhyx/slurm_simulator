# Makefile for priority/carbon plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = priority_carbon.la

# basic priority logging plugin.
priority_carbon_la_SOURCES = priority_carbon.c
priority_carbon_la_LDFLAGS = $(PLUGIN_FLAGS)
