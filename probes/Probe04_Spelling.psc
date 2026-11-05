Scriptname Probe04_Spelling extends ObjectReference
{Probe: which spelling of each identifier the compiler writes when the source spells it differently from the declaration; the listing (Probe04_Spelling.pas.txt) is the answer.}

Int myVar
Probe04_Helper Property Helper Auto
Int Property OwnFull
    Int Function Get()
        Return myVar
    EndFunction
    Function Set(Int aValue)
        myVar = aValue
    EndFunction
EndProperty

Function OwnFunc(Int aParam, ObjectReference aRef = None)
    MYVAR = APARAM
EndFunction

Event oNlOAD()
    ; functions: method, static, own, parent, with odd case
    hELPER.dOtHING(2)
    Int g = pROBE04_hELPER.gLOBALtHING(3)
    oWNfUNC(g)
    uTILITY.wAIT(0.1)
    Actor p = gAME.gETpLAYER()
    pARENT.oNlOAD()
    sELF.dISABLE()
    ; properties: auto and full, another script's and own
    hELPER.aUTOvAL = 1
    hELPER.fULLvAL = 2
    Int total = hELPER.AUTOVAL + hELPER.FULLVAL
    oWNfULL = total
    total = OWNFULL
    ; locals, casts, new, literals
    Int loc = 1
    loc = LOC + 1
    ObjectReference r = p As oBJECTrEFERENCE
    Int[] arr = new iNT[3]
    Bool b = tRUE
    r = nONE
    ; literals that merge with identifiers above
    Debug.Trace("wait" + "disable" + "ownfunc" + "autoval" + "loc")
    GoToState("dONE")
EndEvent

State Done
    Event oNlOAD()
    EndEvent
EndState
