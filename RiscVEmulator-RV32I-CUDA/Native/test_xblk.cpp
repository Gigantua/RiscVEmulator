#include <windows.h>
#include <cstdio>
typedef int(*fn_t)();
int main(){
  HMODULE h=LoadLibraryA("rv32i_cuda.dll"); if(!h){printf("load fail %lu\n",(unsigned long)GetLastError());return 1;}
  fn_t f=(fn_t)GetProcAddress(h,"cuda_rvexec_selftest"); if(!f){printf("proc fail\n");return 1;}
  int r=f(); printf("cuda_rvexec_selftest -> %d (%s)\n", r, r?"PASS":"FAIL");
  fn_t g=(fn_t)GetProcAddress(h,"cuda_rvexec_blocktest"); if(!g){printf("blocktest proc fail\n");return 1;}
  int r2=g(); printf("cuda_rvexec_blocktest -> %d (%s)\n", r2, r2?"PASS":"FAIL");
  return (r&&r2)?0:1;
}
