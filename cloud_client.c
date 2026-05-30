#include "cloud_client.h"

int Cloud_Init(void)
{
    return 0;
}

CloudCommand_t Cloud_DetectCommand(const short *pcm, unsigned long samples)
{
    return CLOUD_COMMAND_AUTHENTICATE;
}

int Cloud_EnrollVoice(const short *pcm, unsigned long samples, int *userId)
{
    return 0;
}

int Cloud_ClearProfiles(void)
{
    return 0;
}

int Cloud_AuthenticateVoice(const short *pcm,
                            unsigned long samples,
                            int *userId,
                            int *score)
{
    return 0;
}
