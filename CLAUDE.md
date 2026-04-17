# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Rakuraku Music Station is a high-performance, lightweight C++ streaming broadcast server designed for low-latency audio distribution. It uses a dual-engine architecture with Linux epoll for streaming and Crow framework for web management.

## Architecture

### Core Components
- **StreamServer**: Manages client socket connections using epoll for efficient data distribution
- **AudioPlayer**: Reads audio files via FFmpeg pipes and feeds data to the broadcast buffer
- **BroadcastBuffer**: Thread-safe ring buffer for producer-consumer data flow
- **WebServer**: Crow-based web interface with authentication and file management
- **AuthMiddleware/SessionManager**: Session-based authentication system for admin access

### Port Configuration
- Web management interface: `2240`
- Audio streaming endpoint: `2241`

## Development Commands

### Building the Project
```bash
# Run the automated build script (handles dependencies and compilation)
./build_release.sh

# The script will:
# 1. Detect system (Arch/Debian/Ubuntu) and install dependencies
# 2. Download Crow framework header (crow_all.h)
# 3. Set up Chinese locale for filename support
# 4. Compile with optimization flags (-O3, -flto, -march=native)
# 5. Create distribution in `dist/` directory
```

### Running the Server
```bash
# Navigate to distribution directory
cd dist

# Add music files to media directory
cp /path/to/your/music/* ./media/

# Start the server
./start.sh

# Stop the server
./stop.sh
```

### Direct Compilation (Development)
```bash
# For development/debug builds
c++ radioserver.cpp -o radioserver -std=c++17 -g -O0 -lpthread -lssl -lcrypto -I.
```

## Key Implementation Details

### Authentication System
- Uses session-based authentication with encrypted cookies
- Admin password configured in `settings.json`
- Middleware automatically applied to crow routes
- Guest access can be enabled/disabled via `allow_guest_skip` setting

### Template System
- HTML templates support variable substitution using `{{VARIABLE_NAME}}` syntax
- Template files are looked up in both current directory and `templates/` directory
- Configuration values from `settings.json` are automatically injected

### Audio Processing
- FFmpeg handles decoding with real-time streaming flags (`-re`)
- Supports MP3, WAV, FLAC, OGG, M4A, AAC formats
- Automatic locale configuration prevents filename encoding issues

### Configuration
Edit `settings.json` to customize:
- Station name and subtitle
- Color scheme (primary, secondary, background)
- Admin password (defaults to "admin123" if not set)
- Guest permissions

## File Structure

```
├── radioserver.cpp          # Main server implementation
├── authmiddleware.hpp       # Authentication middleware
├── sessionmanager.hpp       # Session management
├── settings.json            # Configuration file
├── build_release.sh         # Automated build script
├── dist/                    # Distribution directory
│   ├── radioserver          # Compiled binary
│   ├── start.sh             # Startup script
│   ├── stop.sh              # Shutdown script
│   ├── media/               # Audio files directory
│   └── settings.json        # Copy of configuration
└── templates/               # HTML templates (optional)
    ├── index.html
    ├── admin_panel.html
    └── admin_login.html
```

## Dependencies

The build script automatically handles these dependencies:
- **Arch Linux**: base-devel, ffmpeg, openssl, boost, cmake, wget, asio
- **Debian/Ubuntu**: build-essential, ffmpeg, libavcodec-extra, libssl-dev, libboost-all-dev, locales, wget, libasio-dev

## Troubleshooting

### Common Issues
- **Pipe errors (revents=16)**: Ensure UTF-8 locale is configured and use the provided start.sh script
- **Permission denied**: Build script may require sudo for package installation
- **File not found**: Music files must be placed in `dist/media/` directory
- **Port conflicts**: Check if ports 2240/2241 are already in use

### Debug Mode
For development, compile with debug flags and run directly:
```bash
g++ radioserver.cpp -o radioserver -std=c++17 -g -O0 -lpthread -lssl -lcrypto -I.
./radioserver
```

## Security Notes

- Change the default admin password in production
- Consider firewall configuration for external access
- File uploads are limited to 50MB and supported formats only
- Session cookies are HTTP-only with 24-hour expiration