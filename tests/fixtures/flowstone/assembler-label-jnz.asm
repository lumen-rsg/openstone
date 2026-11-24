streamin in;
streamout out;
float factor=0.5;
mov eax,in[0];
cmp eax,0;
mov edx,2;
jnz start;
mov edx,1;
start:
movaps xmm0,in;
body:
mulps xmm0,factor;
addps xmm0,factor;
sub edx,1;
jnz body;
movaps out,xmm0;
