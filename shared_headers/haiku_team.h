#ifndef __HAIKU_TEAM_H__
#define __HAIKU_TEAM_H__

#include "BeDefs.h"

/* Teams */

typedef struct
{
    team_id team;
    int32 thread_count;
    int32 image_count;
    int32 area_count;
    thread_id debugger_nub_thread;
    port_id debugger_nub_port;
    int32 argc;
    char args[64];
    haiku_uid_t uid;
    haiku_gid_t gid;

    /* Haiku R1 extensions */
    haiku_uid_t real_uid;
    haiku_gid_t real_gid;
    haiku_pid_t group_id;
    haiku_pid_t session_id;
    team_id parent;
    char name[B_OS_NAME_LENGTH];
    bigtime_t start_time;
} haiku_team_info;

#define B_CURRENT_TEAM	0
#define B_SYSTEM_TEAM	1

enum
{
    /* compatible to sys/resource.h RUSAGE_SELF and RUSAGE_CHILDREN */
    B_TEAM_USAGE_SELF       = 0,
    B_TEAM_USAGE_CHILDREN   = -1
};

#endif // __HAIKU_TEAM_H__