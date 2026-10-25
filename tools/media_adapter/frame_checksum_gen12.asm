// Gen12 Xe-LP frame checksum; owned data, no Apple text writes.
// Native source SHA256 3f201b1caba0954e4455e88aeace016859675d4a0db40501ffff632b3781117d
// Decode the source as Gen11: Gen9 silently changes HF/F into UQ/Q.
// Preserve native ALU types, subregisters, regions and control flow.
// Only SEND syntax and generated dependency barriers are adapted.
// Assemble Intel IGA c36a504: -a -p=12p1 -Xauto-deps.
L0:
(W)     shr (1|M0)               r1.8<2>:uw    r0.1<0;1,0>:ud    0xC:uw
(W)     mov (1|M0)               r1.11<1>:hf   0x4:hf
        and (1|M0)    (eq)f0.1   null<1>:uw    r1.8<0;1,0>:uw    0xFFF:uw
(W)     and (1|M0)               r1.10<2>:uw   r0.1<0;1,0>:ud    0xFFF:uw
(W)     mov (1|M0)               r5.4<1>:hf    0x1:hf
(f0.1)  sel (1|M0)               r1.6<1>:d     r1.11<0;1,0>:w    3:w
(~f0.1) sel (1|M0)               r2.1<1>:ud    r5.4<0;1,0>:w     0:w
        shl (1|M0)               r1.7<1>:d     r1.10<0;1,0>:uw   r1.6<0;1,0>:d
        shr (1|M0)               r2.1<1>:ud    r1.3<0;1,0>:ud    r2.1<0;1,0>:ud
        add (1|M0)               r2.0<1>:d     r1.7<0;1,0>:d     7:w
        cmp (1|M0)    (lt)f0.0   null<1>:ud    r2.0<0;1,0>:ud    r2.1<0;1,0>:ud
(W&~f0.0) jmpi                               L1456
L192:
        shr (1|M0)    (eq)f0.0   r2.2<1>:ud    r1.2<0;1,0>:ud    0x3:uw
        mov (1|M0)               r2.3<1>:f     0.0:f
        mov (1|M0)               r2.4<1>:f     0.0:f
        mov (2|M0)               r2.5<1>:f     0.0:f
(W&f0.0) jmpi                                L1104
L272:
(W&f0.1) jmpi                                L416
L288:
(W)     mov (8|M0)               r11.0<1>:f    r0.0<8;8,1>:f                    {Compacted}
(W)     add (1|M0)               a0.0<1>:ud    r1.0<0;1,0>:ud    0x2190001:ud
(W)     mov (1|M0)               r11.2<1>:f    4.34403e-44:f
(W)     mov (1|M0)               r11.0<1>:f    r2.3<0;1,0>:f                    {Compacted}
(W)     mov (1|M0)               r11.1<1>:f    r2.0<0;1,0>:f
(W)     send.dc1 (8|M0) r3 r11 null:ud 0x0 a0.0
        add (1|M0)               r11.0<1>:d    r2.3<0;1,0>:d     32:w
(W)     send.dc1 (8|M0) r4 r11 null:ud 0x0 a0.0
(W)     jmpi                                 L528
L416:
(W)     mov (8|M0)               r12.0<1>:f    r0.0<8;8,1>:f                    {Compacted}
(W)     add (1|M0)               a0.0<1>:ud    r1.0<0;1,0>:ud    0x2190000:ud
(W)     mov (1|M0)               r12.2<1>:f    4.34403e-44:f
(W)     mov (1|M0)               r12.0<1>:f    r2.3<0;1,0>:f                    {Compacted}
(W)     mov (1|M0)               r12.1<1>:f    r2.0<0;1,0>:f
(W)     send.dc1 (8|M0) r3 r12 null:ud 0x0 a0.0
        add (1|M0)               r12.0<1>:d    r2.3<0;1,0>:d     32:w
(W)     send.dc1 (8|M0) r4 r12 null:ud 0x0 a0.0
L528:
        mov (4|M0)               r5.8<1>:w     r2.20<2;1,0>:b
        mov (4|M0)               r13.0<1>:w    r3.8<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r3.0<2;1,0>:b     r5.8<4;4,1>:w
        mov (4|M0)               r13.4<1>:w    r3.9<2;1,0>:b
        mov (4|M0)               r5.12<1>:w    r2.21<2;1,0>:b
        mov (4|M0)               r13.8<1>:w    r3.16<2;1,0>:b
        xor (4|M0)               r2.21<2>:b    r3.1<2;1,0>:b     r5.12<4;4,1>:w
        mov (4|M0)               r13.12<1>:w   r3.17<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r13.0<4;4,1>:w
        mov (4|M0)               r14.0<1>:w    r3.24<2;1,0>:b
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r13.4<4;4,1>:w
        mov (4|M0)               r14.4<1>:w    r3.25<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r13.8<4;4,1>:w
        mov (4|M0)               r14.8<1>:w    r4.0<2;1,0>:b
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r13.12<4;4,1>:w
        mov (4|M0)               r14.12<1>:w   r4.1<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r14.0<4;4,1>:w
        mov (4|M0)               r15.0<1>:w    r4.8<2;1,0>:b
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r14.4<4;4,1>:w
        mov (4|M0)               r15.4<1>:w    r4.9<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r14.8<4;4,1>:w
        mov (4|M0)               r15.8<1>:w    r4.16<2;1,0>:b
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r14.12<4;4,1>:w
        mov (4|M0)               r15.12<1>:w   r4.17<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r15.0<4;4,1>:w
        mov (4|M0)               r16.0<1>:w    r4.24<2;1,0>:b
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r15.4<4;4,1>:w
        mov (4|M0)               r16.4<1>:w    r4.25<2;1,0>:b
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r15.8<4;4,1>:w
        add (1|M0)               r2.4<1>:d     r2.4<0;1,0>:d     1:w
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r15.12<4;4,1>:w
        cmp (1|M0)    (eq)f0.0   null<1>:d     r2.4<0;1,0>:d     r2.2<0;1,0>:d
        xor (4|M0)               r2.20<2>:b    r2.20<2;1,0>:b    r16.0<4;4,1>:w
        add (1|M0)               r2.3<1>:d     r2.3<0;1,0>:d     64:w
        xor (4|M0)               r2.21<2>:b    r2.21<2;1,0>:b    r16.4<4;4,1>:w
(W&~f0.0) jmpi                               L272
L1104:
        xor (1|M0)               r2.7<1>:d     r2.6<0;1,0>:d     r2.5<0;1,0>:d
        xor (1|M0)               r5.0<1>:w     r2.15<0;1,0>:w    r2.14<0;1,0>:w
(W)     mov (1|M0)               r5.5<1>:w     r5.0<0;1,0>:b
        xor (1|M0)               r1.18<2>:b    r5.1<0;1,0>:b     r5.5<0;1,0>:w
(W&f0.1) jmpi                                L1232
L1184:
        mov (8|M0)               r6.0<1>:f     0.0:f
        mov (1|M0)               r6.1<2>:ub    r1.18<0;1,0>:ub
(W)     jmpi                                 L1264
L1232:
        mov (8|M0)               r6.0<1>:f     0.0:f
        mov (1|M0)               r6.0<2>:ub    r1.18<0;1,0>:ub
L1264:
(W)     mov (1|M0)               r5.6<1>:hf    0x8:hf
        mov (8|M0)               r16.8<1>:w    0x76543210:v
(f0.1)  sel (1|M0)               r5.1<1>:d     r5.6<0;1,0>:w     16:w
(W)     add (1|M0)               a0.0<1>:ud    r1.1<0;1,0>:ud    0x2009300:ud
        mul (8|M0)               r7.0<1>:d     r16.8<8;8,1>:w    4:w
        add (8|M0)               r9.0<1>:d     r5.1<0;1,0>:d     r16.8<8;8,1>:w
        send.dc1 (8|M0) null r7 r6:ud 0x40 a0.0
        mov (8|M0)               r8.0<1>:f     0.0:f
        shl (8|M0)               r10.0<1>:d    r9.0<8;8,1>:d     2:w
(W)     add (1|M0)               a0.0<1>:ud    r1.1<0;1,0>:ud    0x2009300:ud
        mov (2|M0)               r8.0<1>:f     r2.5<2;2,1>:f
        send.dc1 (8|M0) null r10 r8:ud 0x40 a0.0
L1456:
(W)     mov (8|M0)               r127.0<1>:f   r0.0<8;8,1>:f                    {Compacted}
(W)     send.ts (8|M0) null r127 null:ud 0x0 0x02000010 {EOT}
L1480:
