#ifndef INSTPOW_H
#define INSTPOW_H
#include <stdint.h>

uint64_t instantPow(int pow);
int getMaxPow(void);
#ifdef INSTPOW_IMPL
uint64_t pows[13] = {
1,
36,
1296,
46656,
1679616,
60466176,
2176782336,
78364164096,
2821109907456,
101559956668416,
3656158440062976,
131621703842267136,
4738381338321616896};

    

int maxPow = 13;
    
        uint64_t instantPow(int pow) {
            if (pow > maxPow) {
                return -1;
            }
        
            return pows[pow];
        }
        
        int getMaxPow(void) {
            return maxPow;
        }
    #endif //INSTPOW_H
#endif //INSTPOW_H
