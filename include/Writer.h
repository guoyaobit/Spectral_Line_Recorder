#pragma once

#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>
#include <map>

class Writer {
private:
    std::ofstream ofs;
    std::map<std::string, std::string> headerMap;
    static constexpr size_t HEADER_SIZE = 4096;
    static constexpr size_t LINE_SIZE = 80;

    // 生成 header ASCII 字符串
    std::string generateHeader() {
        std::ostringstream oss;
        for (const auto& kv : headerMap) {
            std::ostringstream line;
            line << kv.first << " " << kv.second;
            std::string str = line.str();
            if (str.size() > LINE_SIZE) str.resize(LINE_SIZE);
            oss << std::left << std::setw(LINE_SIZE) << str;
        }
        // 填充剩余空格
        std::string header = oss.str();
        if (header.size() < HEADER_SIZE)
            header.append(HEADER_SIZE - header.size(), ' ');
        else if (header.size() > HEADER_SIZE)
            header.resize(HEADER_SIZE);
        return header;
    }

public:
    Writer() = default;

    // 打开文件
    bool open(const std::string& filename) {
        ofs.open(filename, std::ios::binary);
        return ofs.is_open();
    }

    // 设置 header key-value
    void setHeader(const std::string& key, const std::string& value) {
        headerMap[key] = value;
    }

    // 写入 header
    bool writeHeader() {
        if (!ofs.is_open()) return false;
        std::string header = generateHeader();
        ofs.write(header.data(), header.size());
        return ofs.good();
    }

    // 写入数据
    bool writeData(const void* data, size_t size) {
        if (!ofs.is_open()) return false;
        ofs.write(reinterpret_cast<const char*>(data), size);
        return ofs.good();
    }

    // 关闭文件
    void close() {
        if (ofs.is_open()) ofs.close();
    }
};