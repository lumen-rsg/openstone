streamin in;
streamout out;
memrefin buffer;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
float four=4;
jmp main;
runA:
mulps xmm0,half;
addps xmm0,bias;
ret;
runB:
mulps xmm0,two;
subps xmm0,one;
ret;
runC:
addps xmm0,four;
ret;
main:
mov eax,buffer[0];
movaps xmm0,in;
cmp in,0;
jl chooseA;
jz chooseB;
mov ecx,runC;
mov ebx,4;
jmp store;
chooseA:
mov ecx,runA;
mov ebx,0;
jmp store;
chooseB:
mov ecx,runB;
mov ebx,2;
store:
mov [eax+ebx*4],ecx;
mov ebx,0;
mov [eax+ebx*4],ecx;
call [eax+ebx*4];
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
done:
