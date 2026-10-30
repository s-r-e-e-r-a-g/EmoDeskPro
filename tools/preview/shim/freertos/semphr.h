#pragma once
typedef void* SemaphoreHandle_t;
#define portMAX_DELAY 0
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return (void*)1;}
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t,int){return 1;}
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t){return 1;}
