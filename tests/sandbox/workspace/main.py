from http.server import HTTPServer
from routes import AppHandler
from db import init_db

def main():
    init_db()
    server = HTTPServer(("0.0.0.0", 8080), AppHandler)
    print("Server running on port 8080...")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Shutting down...")
        server.server_close()

if __name__ == "__main__":
    main()
