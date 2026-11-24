streamin in;
streamout out;
memrefin buffer;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
jmp main;
runA:
mulps xmm0,half;
addps xmm0,bias;
ret;
runB:
mulps xmm0,two;
subps xmm0,one;
ret;
main:
movaps xmm0,in;
mov ebx,buffer[0];
cmp in,0;
jl chooseA;
mov eax,runB;
mov [ebx],eax;
jmp invoke;
chooseA:
mov eax,runA;
mov [ebx],eax;
invoke:
mov eax,ebx;
call [eax];
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
done:
