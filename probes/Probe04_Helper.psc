Scriptname Probe04_Helper extends ObjectReference
{Helper for Probe04_Spelling: a script with properties and functions another script refers to in other spellings.}

Int Property AutoVal Auto
Int Property FullVal
    Int Function Get()
        Return 1
    EndFunction
    Function Set(Int aValue)
    EndFunction
EndProperty

Function DoThing(Int aCount = 1)
EndFunction

Int Function GlobalThing(Int aCount) Global
    Return aCount
EndFunction
