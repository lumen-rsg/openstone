streamin in;
streamout out;
memrefin buffer;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
float four=4;
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
mov rcx,[eax+ebx*4];
mov [eax+edx*4],rcx;
mov ecx,0;
mov [eax+ebx*4],ecx;
call [eax+edx*4];
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
done:
