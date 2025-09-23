#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
	int num1 = atoi(argv[1]);
	int num2 = atoi(argv[3]);
	char *op = argv[2];
	int result;

	if(strcmp(op, "+") == 0) {
		result = num1 + num2;
	} else if(strcmp(op, "-") == 0) {
		result = num1 - num2;
	} else if(strcmp(op, "x") == 0)	{
		result = num1 * num2;
	} else if(strcmp(op, "/") == 0) {
		if( num2 == 0){
			printf("0 can't devide");
			return 1;
		}
		result = num1 / num2;
	}

	printf("%d\n", result);

	return 0;
}
