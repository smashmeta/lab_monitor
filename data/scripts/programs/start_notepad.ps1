# Runs on each selected host as the lab_monitor agent.
# The exit code decides the outcome: 0 = Completed, anything else = Failed.
# The LM-RESULT line is optional — it lets you say *why*, in the run view.

function Report($ok, $message) {
    $payload = @{ ok = $ok; message = $message } | ConvertTo-Json -Compress
    Write-Output "LM-RESULT: $payload"
}

try {
    # Open a standard Notepad window
	Start-Process -FilePath "notepad.exe"

    # Audible proof this ran on the machine you targeted: one high note for
    # success, one low one for failure. Two sounds that cannot be mistaken
    # for each other, so a row of PCs can be checked by ear. Delete both
    # when you put real work here.
    [console]::Beep(1047, 400)
    Report $true "completed"
    exit 0
}
catch {
    [console]::Beep(196, 700)
    Report $false $_.Exception.Message
    exit 1
}


