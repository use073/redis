//
// Created by liangzaibing on 2024/6/13.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include "../sds.h"
#include "../sdsalloc.h"

int main(){
  sds s = sdsnew("foobar");
  //s[2] = '\0';
  //sdsupdatelen(s);
  //printf("%d\n", sdslen(s));
  return 1;
}