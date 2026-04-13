/*
 * Riz package manager CLI: riz pkg init | add | install
 */

#ifndef RIZ_PKG_H
#define RIZ_PKG_H

int riz_pkg_main(int argc, char** argv);
int riz_install_main(int argc, char** argv);

/* Merged dependency count from riz.json + riz.deps (0 if none / unreadable). */
int riz_pkg_merged_dep_count(void);

#endif
