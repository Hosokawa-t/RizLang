/*
 * Riz — zero-hassle environment helpers (doctor, setup, shell hooks).
 */

#ifndef RIZ_ENV_H
#define RIZ_ENV_H

/* argv / argc are env subcommand tokens (e.g. "setup", "myproj"); program_path is argv[0] from main. */
int riz_env_main(int argc, char** argv, const char* program_path);

#endif
