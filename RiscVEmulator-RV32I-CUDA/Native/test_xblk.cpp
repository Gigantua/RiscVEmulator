#include <windows.h>
#include <cstdio>
typedef int(*fn_t)();
int main(){
  HMODULE h=LoadLibraryA("rv32i_cuda.dll"); if(!h){printf("load fail %lu\n",(unsigned long)GetLastError());return 1;}
  fn_t f=(fn_t)GetProcAddress(h,"cuda_rvexec_selftest"); if(!f){printf("proc fail\n");return 1;}
  int r=f(); printf("cuda_rvexec_selftest -> %d (%s)\n", r, r?"PASS":"FAIL");
  fn_t g=(fn_t)GetProcAddress(h,"cuda_rvexec_blocktest"); if(!g){printf("blocktest proc fail\n");return 1;}
  int r2=g(); printf("cuda_rvexec_blocktest -> %d (%s)\n", r2, r2?"PASS":"FAIL");
  typedef int(*fz_t)(int,int);
  fz_t fz=(fz_t)GetProcAddress(h,"cuda_rvexec_fuzz"); if(!fz){printf("fuzz proc fail\n");return 1;}
  int r3=fz(12345, 400); printf("cuda_rvexec_fuzz -> %d failures (%s)\n", r3, r3==0?"PASS":"FAIL");
  fz_t fz2=(fz_t)GetProcAddress(h,"cuda_rvexec_fuzz2"); if(!fz2){printf("fuzz2 proc fail\n");return 1;}
  int r4=fz2(999, 400); printf("cuda_rvexec_fuzz2 -> %d failures (%s)\n", r4, r4==0?"PASS":"FAIL");
  fz_t fz3=(fz_t)GetProcAddress(h,"cuda_rvexec_fuzz3"); if(!fz3){printf("fuzz3 proc fail\n");return 1;}
  int r5=fz3(7, 300); printf("cuda_rvexec_fuzz3 -> %d failures (%s)\n", r5, r5==0?"PASS":"FAIL");
  return (r&&r2&&r3==0&&r4==0&&r5==0)?0:1;
}
