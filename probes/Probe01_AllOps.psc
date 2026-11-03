Scriptname Probe01_AllOps extends ObjectReference
{Probe: every operator and statement shape, to learn the compiler's lowering and the assembler's syntax.}

Int Property Count Auto
Float Property Speed = 1.5 Auto
Bool Property Ready Auto Hidden
String Property Label = "x" AutoReadOnly
Actor Property Target Auto Conditional
Int Property Computed
    Int Function Get()
        Return Count * 2
    EndFunction
    Function Set(Int value)
        Count = value / 2
    EndFunction
EndProperty

Int hiddenCounter = 7
Float[] samples
String name = "n"

Event OnInit()
    Count = 1
    Speed = 2.0
    name = "hello" + Count + " " + Speed + " " + Ready
EndEvent

Int Function Arith(Int a, Int b, Float f, Float g)
    Int i = a + b - a * b / (a % b)
    Float r = f + g - f * g / (f - g)
    i = -i
    r = -r
    i += 1
    r -= 1.0
    i *= 2
    r /= 2.0
    i %= 3
    Return i + (r as Int)
EndFunction

Bool Function Compare(Int a, Int b, Float f)
    Bool eq = a == b
    Bool ne = a != b
    Bool lt = a < b
    Bool le = a <= b
    Bool gt = a > b
    Bool ge = a >= b
    Bool n = !eq
    Bool both = eq && lt
    Bool either = ne || gt
    Bool mixed = (a < b && f > 1.0) || !ge
    Return both && either || mixed
EndFunction

Function Flow(Int n, ObjectReference ref)
    If n < 0
        Debug.Trace("neg")
    ElseIf n == 0
        Debug.Trace("zero")
    ElseIf n < 10 && ref
        Debug.Trace("small")
    Else
        Debug.Trace("big")
    EndIf
    If ref
        ref.Disable()
    EndIf
    While n > 0
        n -= 1
        If n == 5
            Return
        EndIf
        While n > 7
            n -= 2
        EndWhile
    EndWhile
    Int k = 0
    While k < 3
        k += 1
    EndWhile
EndFunction

Int Function EarlyReturn(Int n)
    If n > 3
        Return 1
    EndIf
    If n > 1
        Return 2
    Else
        Return 3
    EndIf
EndFunction

Function Arrays()
    samples = new Float[5]
    samples[0] = 1.0
    samples[1] = samples[0] + 1.0
    Int len = samples.Length
    Int at = samples.Find(2.0)
    Int rat = samples.RFind(1.0)
    Int at2 = samples.Find(2.0, 1)
    String[] names = new String[2]
    names[0] = name
EndFunction

Function Calls(Actor akActor, Form akForm)
    Parent.OnLoad()
    Self.Disable()
    Disable()
    ObjectReference r = Self as ObjectReference
    Actor a = r as Actor
    akActor.Kill(None)
    Int h = akActor.GetItemCount(akForm)
    Form f = Game.GetFormFromFile(0x000D62, "Skyrim.esm")
    Form g = Game.GetFormFromFile(3426, "Update.esm")
    Float d = Utility.RandomFloat(0.0, 1.0)
    Bool isNone = akForm == None
    Float asf = Count
    Int ci = Speed as Int
    String s = Count as String
    akActor.SetAV("Health", 100)
    Target = akActor
    Int otherCount = (akForm as Probe01_AllOps).Count
    (akForm as Probe01_AllOps).Count = 5
    RegisterForModEvent("MyEvent", "OnMyEvent")
    GoToState("Busy")
    Debug.Trace(GetState())
EndFunction

Event OnMyEvent(String eventName, String strArg, Float numArg, Form sender)
    hiddenCounter = hiddenCounter + 1
EndEvent

Int Function Global1(Int x) Global
    Return x
EndFunction

Auto State Busy
    Event OnActivate(ObjectReference akActionRef)
        GoToState("")
    EndEvent
    Int Function Arith(Int a, Int b, Float f, Float g)
        Return 0
    EndFunction
EndState

State Idle
EndState
