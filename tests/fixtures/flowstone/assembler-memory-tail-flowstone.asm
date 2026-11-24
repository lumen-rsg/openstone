streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
float two=2;
float saved;
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
movaps xmm0,in;
cmp in,0;
jl chooseA;
mov rax,rcx;
mov saved,rax;
jmp restore;
chooseA:
mov rax,rbx;
mov saved,rax;
restore:
mov eax,0;
call outer;
addps xmm0,one;
mulps xmm0,half;
movaps out,xmm0;
jmp done;
outer:
mov rax,saved;
jmp eax;
done:
