// =============================================================================
// agent-lib-MK3 — Server Entry Point
// =============================================================================

#include "src/server/server.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    cortex::mk3::server::ServerConfig cfg;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--host") { if (i+1 < argc) cfg.host = argv[++i]; }
        else if (arg == "--port") { if (i+1 < argc) cfg.port = std::stoi(argv[++i]); }
        else if (arg == "--threads") { if (i+1 < argc) cfg.threads = std::stoi(argv[++i]); }
        else if (arg == "--api-key") { if (i+1 < argc) cfg.apiKey = argv[++i]; }
        else if (arg == "--no-cors") cfg.cors = false;
        else if (arg == "-h" || arg == "--help") {
            std::cout << R"(cortex-mk3-server [options]
  --host <addr>     Bind address (default: 0.0.0.0)
  --port <port>     Bind port (default: 8080)
  --threads <n>     Thread pool size (default: 4)
  --api-key <key>   Bearer token for auth
  --no-cors         Disable CORS
  -h, --help        Show help
)";
            return 0;
        }
    }

    std::cout << "[MK3] Starting server on " << cfg.host << ":" << cfg.port << "\n";
    std::cout << "[MK3] Endpoints:\n";
    std::cout << "  GET  /health\n";
    std::cout << "  GET  /api/v1/agents\n";
    std::cout << "  POST /api/v1/agents         {\"name\":\"...\",\"provider\":\"deepseek\",\"model\":\"deepseek-chat\"}\n";
    std::cout << "  POST /api/v1/agents/:id/prompt  {\"prompt\":\"...\",\"session_id\":\"...\"}\n";
    std::cout << "  POST /api/v1/agents/:id/stream  {\"prompt\":\"...\"}\n";
    std::cout << "  POST /v1/chat/completions   OpenAI-compatible endpoint\n";
    std::cout << "  DEL  /api/v1/agents/:id\n";
    std::cout << "  GET  /api/v1/providers\n";

    cortex::mk3::server::CortexServer server(cfg);
    server.start();

    return 0;
}
