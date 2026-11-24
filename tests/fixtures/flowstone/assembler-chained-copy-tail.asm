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
mov ecx,runA;
mov [eax],ecx;
mov ecx,runB;
mov [eax+8],ecx;
mov ecx,runC;
mov [eax+16],ecx;
movaps xmm0,in;
cmp in,0;
jl chooseA;
jz chooseB;
mov ebx,4;
mov edx,8;
jmp copy;
chooseA:
mov ebx,0;
mov edx,10;
jmp copy;
chooseB:
mov ebx,2;
mov edx,6;
copy:
mov ecx,[eax+ebx*4];
mov [eax+edx*4],ecx;
mov ecx,0;
mov [eax+ebx*4],ecx;
mov ebx,edx;
mov edx,12;
mov ecx,[eax+ebx*4];
mov [eax+edx*4],ecx;
mov ecx,0;
mov [eax+ebx*4],ecx;
mov ebx,edx;
mov edx,14;
mov ecx,[eax+ebx*4];
mov [eax+edx*4],ecx;
mov ecx,0;
mov [eax+ebx*4],ecx;
call outer;
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
outer:
jmp [eax+edx*4];
done:
