#include <stdio.h>

#include "untrained_model.h"

int main(void)
{
#if AI_SCHED_MODEL_AVAILABLE
	return 0;
#else
	(void)puts("AI_SCHED_V1 model-unavailable observe-only");
	return 0;
#endif
}
