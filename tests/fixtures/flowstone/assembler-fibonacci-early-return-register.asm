streamin in;
streamout out;
float eight=8;
movaps xmm1,in;
addps xmm1,eight;
cvttps2dq xmm1,xmm1;
movd edx,xmm1;
mov eax,fib;
call eax;
movd xmm0,ebx;
cvtdq2ps xmm0,xmm0;
movaps out,xmm0;
jmp done;
fib:
mov ebx,edx;
cmp edx,1;
jg recurse;
ret;
recurse:
push edx;
sub edx,1;
call eax;
pop edx;
push ebx;
sub edx,2;
call eax;
pop ecx;
add ebx,ecx;
ret;
done:
