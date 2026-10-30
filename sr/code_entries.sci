; Code entry points that no segment fixup refers to.
;
; In real mode a program installs interrupt handlers (and takes addresses from
; tables) at run time, so a static pass over the relocation entries never sees
; those entry points.  Every label listed here starts a piece of code; the
; recompiler disassembles from it in addition to the addresses it finds itself.
;
; The addresses are guest addresses (image base + image offset), the same ones
; the trace and the generated assembly use.

; int 08h replacement installed by the routine at 0000:2902
loc_2902

; VGA blitting helpers, reached only through a table of far pointers
loc_4D00
loc_4D99
loc_4E45
loc_4EF6

; far call the ordinary control flow never walks over (it follows a retf)
loc_7C8CF

; mouse driver callbacks
loc_830C1
loc_83141
loc_83272
loc_836D5
; ... and the handlers the callback table at 7dd3:39d2 points at
loc_8373E
loc_837D3
loc_837D9
loc_837DF
loc_8386A
loc_83896

; mouse driver entry points
loc_8E5D4
loc_8E5E0
loc_8E61F

; routine that installs the int 08h handler at 8c1f:08da
loc_8E5AD
loc_8E5BC

; screen update routine, called from a table
loc_11605

; int 09h (keyboard) handler
loc_11C51
loc_11D66

; interrupt handlers of the block at 1353:0000 (timer, keyboard, ...)
loc_155A0
loc_155D3

; ---------------------------------------------------------------------------
; Functions that only exist between a return and the next function and that no
; static branch reaches: they are called through tables or from the areas above.
; ---------------------------------------------------------------------------
loc_2841
loc_6964
loc_69DB
loc_69FB
loc_6C51
loc_6CDC
loc_F565
loc_11599
loc_11B22
loc_11B47
loc_11B6B
loc_7C0A1
loc_7C0B3
loc_7CD27
loc_7E1A9
loc_7E1AA
loc_7F5AC
loc_8328A
loc_83A83
loc_83B1D
loc_8E44E
loc_8E458
loc_8E460
loc_8E468
loc_8EB3A
loc_8ECA0
loc_8ECB4
loc_8ECB9
loc_8ECBE
loc_8ECC3
loc_8ED97

; ---------------------------------------------------------------------------
; The mouse driver callback table at 7dd3:3797 (0000:83557 dispatches through
; "jmp word near [cs:bp+0x3797]", 16 near offsets in segment 7dd3).
; ---------------------------------------------------------------------------
loc_83572
loc_835C1
loc_83624
loc_83667
loc_836D5
loc_8373E
loc_837D3
loc_837D9
loc_837DF
loc_8386A
loc_83936

; the smaller callback table at 7dd3:3b8c (5 entries, all targets)
loc_838BB
loc_838E2
loc_83902

; ---------------------------------------------------------------------------
; The Adlib/Sound Blaster parameter table at 7dd3:3ba6.
;
; 0000:83936 loads the high nibble of [si+8], doubles it and does
; "jmp word near [cs:bx+0x3ba6]"; the table holds 16 near offsets in segment
; 7dd3.  Several entries point at the same one-byte "ret" (3bc6/3bc7), the
; rest at small handlers.  The comment is the offset as the table stores it.
; ---------------------------------------------------------------------------
loc_83618   ; 3878
loc_8365B   ; 38bb
loc_83966   ; 3bc6  ret (also 3bc7, a second ret right after it)
loc_83967   ; 3bc7
loc_83968   ; 3bc8
loc_83976   ; 3bd6
loc_83977   ; 3bd7
loc_83988   ; 3be8
loc_839E8   ; 3c48
loc_839F9   ; 3c59
loc_83A09   ; 3c69
loc_83A20   ; 3c80
loc_83A37   ; 3c97

; ---------------------------------------------------------------------------
; The near call table at 18f0:0000.
;
; 0000:a430 does "mov ax, es:[si]" over the table at es=18f0, index [0x2094]
; (0..0x59) and then "call ax" with CS still 0, so every non-zero word is a
; near offset in code segment 0.  The list below is that table, duplicate
; values dropped; the comment is the offset as the table stores it.
; ---------------------------------------------------------------------------
loc_0A45E   ; 83ee
loc_0A4DA   ; 846a
loc_0A564   ; 84f4
loc_0A5A9   ; 8539
loc_0A5EE   ; 857e
loc_0A635   ; 85c5
loc_0A6C6   ; 8656
loc_0A780   ; 8710
loc_0BC7F   ; 9c0f
loc_0BE2F   ; 9dbf
loc_0BF54   ; 9ee4
loc_0C092   ; a022
loc_0C25C   ; a1ec
loc_0C25D   ; a1ed
loc_0C39B   ; a32b
loc_0C4E5   ; a475
loc_0C6E4   ; a674
loc_0C6E5   ; a675
loc_0C7ED   ; a77d
loc_0C912   ; a8a2
loc_0CA37   ; a9c7
loc_0CB1D   ; aaad
loc_0CC04   ; ab94
loc_0CD1A   ; acaa
loc_0CDE2   ; ad72
loc_0CEB6   ; ae46
loc_0CF73   ; af03
loc_0D043   ; afd3
loc_0D044   ; afd4
loc_0D114   ; b0a4
loc_0D116   ; b0a6
loc_0D1D0   ; b160
loc_0D243   ; b1d3
loc_0D2B6   ; b246
loc_0D329   ; b2b9
loc_0D39C   ; b32c
loc_0D40F   ; b39f
loc_0D482   ; b412
loc_0D4F5   ; b485
loc_0D568   ; b4f8
loc_0D5DB   ; b56b
loc_0D64E   ; b5de
loc_0D6C1   ; b651
loc_0D734   ; b6c4
loc_0D7A7   ; b737
loc_0D81A   ; b7aa
loc_0D88D   ; b81d
