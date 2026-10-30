#pragma once
struct FakeFS { bool remove(const char*) { return true; } };
static FakeFS LittleFS;
