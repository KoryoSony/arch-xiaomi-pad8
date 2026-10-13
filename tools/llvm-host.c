// llvm-host: print what LLVM detects as the host CPU (debug helper).
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
int main(void)
{
	printf("cpu: %s\nfeatures: %s\n", LLVMGetHostCPUName(), LLVMGetHostCPUFeatures());
	return 0;
}
