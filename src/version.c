#include <stdio.h>
#include <string.h>
#include "version.h"
#include "log.h"

int cyber_parse_version(const char *user_agent, CyberVersion *out) {
    if (!user_agent || !out) return 0;

    out->build  = 0.0f;
    out->season = 0;
    out->cl     = 0;

    /* Look for the "Release-" tag in the UA string.
       Fortnite sends something like:
       "Fortnite/++Fortnite+Release-33.00-CL-38383838 ..." */
    const char *release = strstr(user_agent, "Release-");
    if (!release) {
        cyber_warn("Could not find Release- in User-Agent: %s", user_agent);
        return 0;
    }

    release += 8; /* skip "Release-" */

    float build = 0.0f;
    if (sscanf(release, "%f", &build) != 1) {
        cyber_warn("Could not parse build number from: %s", release);
        return 0;
    }

    out->build  = build;
    out->season = (int)build;

    /* Try to parse the CL number after "-CL-" */
    const char *cl_tag = strstr(user_agent, "-CL-");
    if (cl_tag) {
        cl_tag += 4;
        sscanf(cl_tag, "%d", &out->cl);
    }

    return 1;
}
