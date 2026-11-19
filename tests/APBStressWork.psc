Scriptname APBStressWork Hidden

Int Function Run(Int slices) Global
    Int i = 0
    Int completed = 0
    While i < slices
        completed += WorkSlice()
        i += 1
    EndWhile
    Return completed
EndFunction

Int Function WorkSlice() Global Native
