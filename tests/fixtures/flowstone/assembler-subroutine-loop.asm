streamin in;
streamout out;
float half=0.5;
float bias=0.25;
float one=1;
movaps xmm0,in;
mov eax,adjust;
mov edx,in[0];
cmp edx,0;
mov edx,2;
jnz firstReady;
mov edx,0;
firstReady:
call eax;
addps xmm0,one;
mov edx,3;
call eax;
movaps out,xmm0;
jmp done;
adjust:
cmp edx,0;
jz returned;
body:
mulps xmm0,half;
addps xmm0,bias;
sub edx,1;
jnz body;
returned:
ret;
done:
