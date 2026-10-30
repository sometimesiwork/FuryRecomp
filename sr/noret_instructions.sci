; Instructions after which control never continues.
;
; The transcriber walks forward from an entry point and only stops at an
; instruction that another piece of code already decoded.  A DOS terminate call
; is the end of its routine, and what follows it in the image is often a string
; or a table; without a note here the walk would decode that data as code.
;
; Every line is the address of the instruction itself (the one that does not
; return), not of its target.

; Adlib initialisation: the error path prints a message and terminates
; (0000:83B5D "int 21h" with ah=9 prints it, 0000:83B62 exits with al=0xff).
loc_83B62
