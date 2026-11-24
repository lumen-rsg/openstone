streamin in;
streamout out;
memrefin buffer;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
float four=4;
int leftIndex;
int rightIndex;
call bindA;
runA:
mulps xmm0,half;
addps xmm0,bias;
ret;
bindA:
pop rbx;
call bindB;
runB:
mulps xmm0,two;
subps xmm0,one;
ret;
bindB:
pop rcx;
call bindC;
runC:
addps xmm0,four;
ret;
bindC:
pop rdx;
mov rax,buffer[0];
mov [eax],rbx;
mov [eax+8],rcx;
mov [eax+16],rdx;
movaps xmm0,in;
cmp in,0;
jl chooseAB;
jz chooseBC;
mov ebx,4;
mov edx,0;
jmp swap;
chooseAB:
mov ebx,0;
mov edx,2;
jmp swap;
chooseBC:
mov ebx,2;
mov edx,4;
swap:
mov leftIndex,ebx;
mov rightIndex,edx;
mov rcx,[eax+ebx*4];
mov rdx,[eax+edx*4];
mov [eax+ebx*4],rdx;
mov ebx,rightIndex;
mov [eax+ebx*4],rcx;
mov ecx,0;
mov edx,0;
mov ebx,leftIndex;
call [eax+ebx*4];
mov ebx,rightIndex;
call [eax+ebx*4];
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
done:
