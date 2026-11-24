streamin in;
streamout out;
float step=0.5;
mov eax,in[0];
cmp eax,0;
mov edx,4;
jnl start;
mov edx,2;
start:
movaps xmm0,in;
body:
addps xmm0,step;
sub edx,1;
jz done;
jmp body;
done:
movaps out,xmm0;
