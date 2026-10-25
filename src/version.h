#ifndef CYBER_VERSION_H
#define CYBER_VERSION_H

/* Parsed version info from the Fortnite User-Agent header.
   Example UA: "Fortnite/++Fortnite+Release-33.00-CL-38383838" */
typedef struct {
    float  build;       /* e.g. 33.00 */
    int    season;      /* e.g. 33 -> season 33 */
    int    cl;          /* changelist number */
} CyberVersion;

/* Parse the User-Agent string and fill out a CyberVersion.
   Returns 1 on success, 0 if the header was missing or unparseable. */
int cyber_parse_version(const char *user_agent, CyberVersion *out);

#endif
