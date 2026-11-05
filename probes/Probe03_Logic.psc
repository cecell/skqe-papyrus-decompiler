Scriptname Probe03_Logic extends ObjectReference
{Probe: the temp an && / || accumulator takes, with the bool free list in known states; the listing (Probe03_Logic.pas.txt) is the answer.}

Bool Function F1()
    Return True
EndFunction

Bool Function F2()
    Return False
EndFunction

; Q0: empty free list.
Function Q0(Int a, Int b)
    Bool r = a == 1 || b == 2
EndFunction

; Q1: one free bool temp (p's comparison temp) before the ||.
Function Q1(Int a, Int b)
    Bool p = a == 7
    Bool r = a == 1 || b == 2
EndFunction

; Q2: two free bool temps, released in a known order (the first comparison's, then the second's).
Function Q2(Int a, Int b)
    Bool p = (a == 7) == (b == 8)
    Bool r = a == 1 || b == 2
EndFunction

; Q3: the left operand is a call result.
Function Q3(Int a, Int b)
    Bool p = a == 7
    Bool r = F1() || b == 2
EndFunction

; Q4: the left operand is a variable (no temp).
Function Q4(Int a, Int b, Bool f)
    Bool p = a == 7
    Bool r = f || b == 2
EndFunction

; Q5: the right operand is a call result.
Function Q5(Int a, Int b)
    Bool p = a == 7
    Bool r = a == 1 || F1()
EndFunction

; Q6: the right operand is a comparison with a call inside.
Function Q6(Int a, Int b, Bool f)
    Bool p = a == 7
    Bool r = a == 1 || F1() == f
EndFunction

; Q7: nested chains, both ways.
Function Q7(Int a, Int b, Int c)
    Bool p = a == 7
    Bool r = (a == 1 || b == 2) || c == 3
    Bool s2 = a == 1 || (b == 2 || c == 3)
    Bool t2 = a == 1 && b == 2 || c == 3
EndFunction

; Q8: && / || as If and ElseIf conditions, with bool temps in the bodies and after.
Function Q8(Int a, Int b, Int c, Bool f)
    If a == 1 && b == 2
        Bool x1 = c == 1
    ElseIf c == 3 && f
        Bool x2 = c == 2
    ElseIf F1() || F2()
        Bool x3 = c == 3
    EndIf
    Bool y1 = a == 4
    Bool y2 = a == 5
    Bool y3 = a == 6
EndFunction

; Q9: operands that need a cast to bool (ints), and a bool compared with an int literal.
Function Q9(Int a, Int b)
    Bool p = a == 7
    Bool r = a || b
    Bool s2 = F1() == 1 || F2() == 0
EndFunction

; Q10: the left comparison's temp is not at the front of the free list.
Function Q10(Int a, Int b)
    Bool p = (a == 7) == (b == 8)
    Bool q = a == 9
    Bool r = a == 1 || b == 2
EndFunction

; Qa..Qj: the same shapes under different free lists, to separate shape from state.
Function Qa(Int a, Int b, Int c)
    Bool r = a == 1 || (b == 2 || c == 3)
EndFunction

Function Qb(Int a, Int b, Int c)
    Bool p = a == 7
    Bool r = a == 1 || (b == 2 || c == 3)
EndFunction

Function Qc(Int a, Int b, Int c)
    Bool p = (a == 7) == (b == 8)
    Bool r = a == 1 || (b == 2 || c == 3)
EndFunction

Function Qd(Int a, Int b, Int c)
    Bool p = a == 7
    Bool r = (a == 1 && b == 2) || c == 3
EndFunction

Function Qe(Int a, Int b)
    Bool r = F1() == 1 || F2() == 0
EndFunction

Function Qg(Int a, Int b)
    Bool p = (a == 7) == (b == 8)
    Bool r = F1() || b == 2
EndFunction

Function Qh(Int a, Int b, Bool f)
    Bool p = (a == 7) == (b == 8)
    Bool r = f || b == 2
EndFunction

Function Qi(Int a, Int b, Int c)
    Bool r = a == 1 || b == 2 || c == 3
EndFunction

Function Qj(Int a, Int b, Int c)
    Bool p = (a == 7) == (b == 8)
    Bool r = a == 1 || b == 2 || c == 3
EndFunction

Function Qk(Int a, Int b, Int c)
    Bool p = (a == 7) == (b == 8)
    Bool r = (a == 1 && b == 2) || c == 3
EndFunction
