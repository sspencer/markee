#ifndef MARKEE_CLI_H
#define MARKEE_CLI_H

#include <stdbool.h>

/* Return true if arg is a client CLI subcommand ("say", "notify", "gif", "timer",
 * "clock", "alert", "clear", "info"). */
bool cli_is_subcommand(const char *arg);

/* Parse a human duration string such as "90", "90s", "25m", "1h30m" into seconds (> 0). */
bool cli_parse_duration_sec(const char *s, double *out_sec);

/* Execute a client CLI subcommand against a running markee server. Returns exit code (0 = ok). */
int cli_run(int argc, char **argv);

#endif /* MARKEE_CLI_H */
