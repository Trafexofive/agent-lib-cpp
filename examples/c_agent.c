/*
 * Cortex-Prime C API Example
 *
 * Build: gcc -o c_example c_example.c -I../include -L.. -lagent-mk2 -lcurl -lyaml-cpp -ljsoncpp -lpthread
 * Run:   LD_LIBRARY_PATH=.. ./c_example
 */

#include "cortex/cortex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Streaming callback — prints each token as it arrives */
static int on_token(const char* token, size_t len, void* user_data) {
    (void)user_data;
    fwrite(token, 1, len, stdout);
    fflush(stdout);
    return 0; /* 0 = continue, non-zero = interrupt */
}

int main(int argc, char** argv) {
    const char* manifest = "config/agents/assistant/agent.yml";
    const char* provider = NULL;   /* use manifest default */
    const char* model = NULL;      /* use manifest default */
    const char* prompt = "What is 2+2?";

    /* Override from CLI args */
    if (argc > 1) prompt = argv[1];
    if (argc > 2) provider = argv[2];
    if (argc > 3) model = argv[3];

    printf("Cortex-Prime v%s\n", cortex_version());

    /* Create agent */
    cortex_error_t error;
    cortex_agent_t* agent = cortex_agent_create(manifest, provider, model, &error);
    if (!agent) {
        fprintf(stderr, "Failed to create agent: [%d] %s\n", error.code, error.message);
        return 1;
    }

    /* List loaded tools */
    const char* tool_names[32];
    int tool_count = cortex_agent_list_tools(agent, tool_names, 32);
    printf("\nTools (%d):", tool_count > 32 ? 32 : tool_count);
    for (int i = 0; i < tool_count && i < 32; i++) {
        printf(" %s", tool_names[i]);
    }
    printf("\n\n");

    /* Send prompt with streaming */
    printf("Response: ");
    char* result = cortex_agent_prompt_stream(agent, prompt, on_token, NULL, "demo_session", &error);
    printf("\n\n");

    if (result) {
        printf("Complete response (%zu bytes)\n", strlen(result));
        cortex_free(result);
    } else {
        fprintf(stderr, "Prompt failed: [%d] %s\n", error.code, error.message);
    }

    /* Direct tool dispatch — bypass LLM */
    char* tool_result = NULL;
    cortex_err_t err = cortex_agent_dispatch_tool(
        agent, "list", "{\"path\":\".\"}", &tool_result, &error);
    if (err == CORTEX_OK && tool_result) {
        printf("\nDirect tool call (list): %s\n", tool_result);
        cortex_free(tool_result);
    }

    /* Clean up */
    cortex_agent_destroy(agent);
    printf("\nDone.\n");
    return 0;
}
