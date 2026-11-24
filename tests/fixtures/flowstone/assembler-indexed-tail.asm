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
movaps xmm0,in;
mov eax,buffer[0];
mov ecx,runA;
mov [eax],ecx;
mov ecx,runB;
mov [eax+8],ecx;
mov ecx,runC;
mov [eax+16],ecx;
cmp in,0;
jl chooseA;
jz chooseB;
mov ebx,4;
jmp invoke;
chooseA:
mov ebx,0;
jmp invoke;
chooseB:
mov ebx,2;
invoke:
call outer;
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
outer:
jmp [eax+ebx*4];
done:
