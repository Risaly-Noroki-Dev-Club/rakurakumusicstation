#include "RadioServer.hpp"

int main() {
    try {
        RadioServer server;
        server.start();
        
        // 等待退出信号
        std::cout << "Radio server started\n";
        std::cout << "Web UI: http://localhost:2240\n";
        std::cout << "Stream: http://localhost:2241\n";
        
        // 保持主线程运行
        while (true) std::this_thread::sleep_for(1s);
    }
    catch (const exception& e) {
        cerr << "Fatal error: " << e.what() << endl;
        return 1;
    }
    return 0;
}
