# QtLog - A Thread-Safe Logging System for Qt Applications

`QtLog` is a simple and efficient logging system for Qt applications, designed to handle high-frequency log writes without blocking the main thread. It uses a thread-safe singleton pattern and allows logs to be written asynchronously in a separate worker thread. This system supports logging to both files and the console, with configurable log levels (Info, Warning, Error).

## Features
- **Thread-Safe**: Ensures that log messages are written safely from multiple threads.
- **Asynchronous Logging**: Uses a separate worker thread for log writing to minimize main thread blocking.
- **Multiple Output Options**: Supports writing logs to both files and the console.
- **Log Levels**: Allows filtering of log messages by severity (Info, Warning, Error).
- **Singleton Pattern**: Manages log instances per file path using a thread-safe singleton pattern.

## Prerequisites
- Qt 6.x or higher
- C++11 or higher

## Installation

1. Clone the repository to your local machine:
   git clone https://github.com/your-username/QtLog.git

2. Open the project in Qt Creator or your preferred IDE.

3. Build the project using the Release or Debug configuration as required.

# Usage
## 1. Initialize the Logger

To start logging, call the QtLog::Instance method with the desired log file path.
    
    QtLog *logger = QtLog::Instance("/path/to/your/logfile.log");

## 2. Write Logs

You can write logs using the writeLog method, specifying the log message, log level, and output options.

    logger->writeLog("This is an info message", QwtLogger::Info, QwtLogger::FileAndConsole);
    logger->writeLog("This is a warning message", QwtLogger::Warning, QwtLogger::ConsoleOnly);
    logger->writeLog("This is an error message", QwtLogger::Error, QwtLogger::FileOnly);

### Available Log Levels:
- QwtLogger::Info: Informational messages.
- QwtLogger::Warning: Warning messages.
- QwtLogger::Error: Error messages.

### Available Output Options:
- QwtLogger::FileOnly: Log message only to file.
- QwtLogger::ConsoleOnly: Log message only to console.
- QwtLogger::FileAndConsole: Log message to both file and console.

## 3. Customize Logging Behavior
You can change the behavior of logging by modifying the LogWorker class. It processes the log messages and writes them to the specified file. You can also adjust the log message format or add additional log levels by modifying the corresponding methods.

## 4. Clean Shutdown
The logger gracefully shuts down when the application exits, ensuring that all log messages are written and no resources are left open.

    QtLog::Instance("/path/to/your/logfile.log")->~QtLog();
    
### Architecture
The QtLog logging system operates using a worker thread that handles log writing asynchronously. The main thread sends log messages via Qt's signal-slot mechanism. This ensures that the main thread is not blocked by frequent log writes.

### Components
QtLog Class: The main logging interface. It manages log instances and sends log messages to the worker thread.
LogWorker Class: A worker class that runs in a separate thread and processes the log messages.
Singleton Pattern: Ensures only one instance of the logger per file path.
Example
Here is a complete example that demonstrates how to use QtLog in your Qt application:

    #include <QCoreApplication>
    #include "QtLog.h"

    int main(int argc, char *argv[])
    {
        QCoreApplication a(argc, argv);
    
        // Initialize the logger
        QtLog *logger = QtLog::Instance("app.log");
    
        // Write some log messages
        logger->writeLog("This is an informational message", QwtLogger::Info, QwtLogger::FileAndConsole);
        logger->writeLog("This is a warning message", QwtLogger::Warning, QwtLogger::ConsoleOnly);
        logger->writeLog("This is an error message", QwtLogger::Error, QwtLogger::FileOnly);
    
        return a.exec();
    }
    
### License
This project is licensed under the MIT License - see the LICENSE file for details.
