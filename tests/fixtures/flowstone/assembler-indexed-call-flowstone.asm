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
mov rdx,buffer[0];
mov [rdx],rbx;
mov [rdx+8],rcx;
call bindC;
runC:
addps xmm0,four;
ret;
bindC:
pop rcx;
mov [rdx+16],rcx;
mov rax,rdx;
movaps xmm0,in;
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
call [eax+ebx*4];
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
done:
