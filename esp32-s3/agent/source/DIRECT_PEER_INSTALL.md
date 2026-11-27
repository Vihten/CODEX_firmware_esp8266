# Direct peer chat on Windows

## For PCs updated from the controller web page

Download the matching published agent bootstrap archive and extract it. In an
Administrator terminal opened in the extracted folder, run one command:

    .\configure_peer_firewall.cmd

Run this on each participating PC that should accept incoming peer traffic.
The helper can run directly from the extracted folder. Running the manual
installer again is unnecessary. Administrative privileges apply to firewall
changes. Normal agent operation and peer chat run as the signed-in user.

The helper configures two inbound Windows PowerShell rules. Discovery uses
UDP 4214 and chat uses TCP 4215. Both rules apply only to Domain and Private
profiles, limit remote addresses to LocalSubnet, and block edge traversal.
The peer protocol authenticates the connection separately. Preview the
operation in an ordinary terminal with:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\configure_peer_firewall.ps1 -Action Enable -WhatIf

To remove the two rules, use an Administrator terminal:

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\configure_peer_firewall.ps1 -Action Remove

## Manual installation

Extract the complete bootstrap. Run install_codex_lan_agent.cmd for a new
installation or update_codex_lan_agent.cmd for an existing installation with
saved connection settings. Both launch Windows PowerShell. From PowerShell,
the corresponding .ps1 scripts also work directly. Run these as the regular
Windows user who will run the agent.

The editable source has a separate agent_chat_peer.ps1 beside
agent_chat_ipc.ps1. Keep both when running directly from source. The release
builder and source installer embed the peer functions into agent_chat_ipc.ps1
for installed use. The existing signed web updater still verifies exactly ten
files. Firewall setup is a separate, explicit operation.
