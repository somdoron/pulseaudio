#ifndef fooprotocolnativespech
#define fooprotocolnativespech

enum {
    PA_COMMAND_ERROR,
    PA_COMMAND_TIMEOUT, /* pseudo command */
    PA_COMMAND_REPLY,
    PA_COMMAND_CREATE_PLAYBACK_STREAM,
    PA_COMMAND_DELETE_PLAYBACK_STREAM,
    PA_COMMAND_CREATE_RECORD_STREAM,
    PA_COMMAND_DELETE_RECORD_STREAM,
    PA_COMMAND_EXIT,
    PA_COMMAND_REQUEST,
    PA_COMMAND_MAX
};

enum {
    PA_ERROR_OK,
    PA_ERROR_ACCESS,
    PA_ERROR_COMMAND,
    PA_ERROR_INVALID,
    PA_ERROR_EXIST,
    PA_ERROR_NOENTITY,
    PA_ERROR_CONNECTIONREFUSED,
    PA_ERROR_PROTOCOL,
    PA_ERROR_TIMEOUT
};

#endif
