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
movaps xmm0,in;
cmp in,0;
jl chooseA;
jz chooseB;
mov rcx,rdx;
mov ebx,4;
jmp store;
chooseA:
mov rcx,rbx;
mov ebx,0;
jmp store;
chooseB:
mov ebx,2;
store:
mov [eax+ebx*4],rcx;
mov ebx,0;
mov [eax+ebx*4],rcx;
call outer;
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
outer:
jmp [eax+ebx*4];
done:
