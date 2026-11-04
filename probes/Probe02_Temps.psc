Scriptname Probe02_Temps extends ObjectReference
{Probe: the compiler's temp allocator. Every function isolates one question; the listing (Probe02_Temps.pas.txt) is the answer.}

Int Function GetCount()
    Return 3
EndFunction

Function Take(Int a, Int b, Int c)
EndFunction

String Function Name()
    Return "n"
EndFunction

; 1. Free list order: three int temps freed in a known order, then allocations.
;    (a+b) is held while (b+c) and (c+a) are made; the sum frees them.
Int Function Order(Int a, Int b, Int c)
    Int bx = (a + b) + ((b + c) + (c + a))
    Int by = a * b
    Int bz = b * c
    Return bx + by + bz
EndFunction

; 2. The ElseIf chain: when is each condition temp released?
;    Same-type (bool) allocations inside every branch show what is free there.
Function Chain(Int a, Int b, Int c)
    If a == b
        Bool bx = c == a
        Take(1, 1, 1)
    ElseIf a == c
        Bool by = b == c
        Take(2, 2, 2)
    ElseIf b == c
        Bool by2 = a == 1
        Take(3, 3, 3)
    Else
        Bool bz = a == 2
        Take(4, 4, 4)
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
EndFunction

; 3. The same chain written as nested Else / If.
Function Nested(Int a, Int b, Int c)
    If a == b
        Bool bx = c == a
        Take(1, 1, 1)
    Else
        If a == c
            Bool by = b == c
            Take(2, 2, 2)
        Else
            If b == c
                Bool by2 = a == 1
                Take(3, 3, 3)
            Else
                Bool bz = a == 2
                Take(4, 4, 4)
            EndIf
        EndIf
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
EndFunction

; 4. Conditions of other types and with && / ||, in a chain.
Function Types(Int a, ObjectReference ref)
    If GetCount()
        Int ip = GetCount()
    ElseIf GetCount() + 1
        Int iq = GetCount()
    ElseIf a < 1 && ref
        Bool br = a == 5
    ElseIf a < 2 || a > 9
        Bool br2 = a == 6
    EndIf
    Int it = GetCount()
    Bool bu = a == 7
EndFunction

; 5. While: its condition temp, an If inside, same-type temps in both bodies.
Function Loop(Int a, Int b)
    While a < b
        Bool bx = a == b
        If a == 1
            Bool by = b == 1
        EndIf
        Bool bz = a == 2
        a += 1
    EndWhile
    Bool bw = a == 3
EndFunction

; 6. Call arguments: temps for three arguments, then allocations after the call.
Function Args(Int a, Int b, Int c)
    Take(a + b, b + c, c + a)
    Int bx = a + b
    Int by = b + c
EndFunction

; 7. String concatenation with casts on both sides and a call result on the right.
Function Strings(Int a, Float f)
    String st = a + Name()
    String st2 = Name() + a
    String st3 = a + " " + f
    String st4 = (a as String) + Name() + (f as String)
EndFunction

; 8. An If with no ElseIf: the condition temp held through the body and the else.
Function Plain(Int a, Int b)
    If a == b
        Bool bx = a == 1
    Else
        Bool by = a == 2
    EndIf
    Bool bz = a == 3
    If a < b
        Bool bx2 = a == 4
    EndIf
    Bool bz2 = a == 5
EndFunction

; 9. The end of a chain without Else: which is released first, the If's temp or the
;    last ElseIf's? Variants: one or four ElseIfs, a plain or an || condition last,
;    the chain in a then-branch or in an Else, and an empty Else.
Function EndOne(Int a, Int b, Int c)
    If a == b
        Take(1, 1, 1)
    ElseIf a == c
        Take(2, 2, 2)
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
EndFunction

Function EndFour(Int a, Int b, Int c)
    If a == b
        Take(1, 1, 1)
    ElseIf a == c
        Take(2, 2, 2)
    ElseIf b == c
        Take(3, 3, 3)
    ElseIf a == 1
        Take(4, 4, 4)
    ElseIf a == 2
        Take(5, 5, 5)
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
    Bool bw3 = a == 5
EndFunction

Function EndOr(Int a, Int b, Int c)
    If a == b
        Take(1, 1, 1)
    ElseIf a == c || b == c
        Take(2, 2, 2)
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
EndFunction

Function EndInElse(Int a, Int b, Int c)
    If a == 9
        Take(0, 0, 0)
    Else
        If a == b
            Take(1, 1, 1)
        ElseIf a == c
            Take(2, 2, 2)
        EndIf
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
    Bool bw3 = a == 5
EndFunction

Function EndEmptyElse(Int a, Int b, Int c)
    If a == b
        Take(1, 1, 1)
    ElseIf a == c
        Take(2, 2, 2)
    Else
        ; nothing
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
EndFunction

Function EndInThen(Int a, Int b, Int c)
    If a == 9
        If a == b
            Take(1, 1, 1)
        ElseIf a == c
            Take(2, 2, 2)
        EndIf
    EndIf
    Bool bw = a == 3
    Bool bw2 = a == 4
    Bool bw3 = a == 5
EndFunction
