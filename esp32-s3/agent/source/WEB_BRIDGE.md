## November 2026 web migration

On Windows with Python 3.12, the Python cryptography package, Git, and Windows
PowerShell 5.1, run the following from the D:/CodexApp workspace.

    python tool/build_agent_web_bridge.py
    python tools/codex-lan-agent/test_web_bridge.py

The builder reads the signed eight-file package and public key from release
commit 5a3ea7f9fc3dfe114e444eb76f1a1121a3759e2a, the parent of the chat
release. It selects the existing publisher key by its public parameters.
The output pair is in build/agent-web-bridge-20261127/bridge. Both files
belong to the same build. The final chat pair is in
build/agent-release-chat-20261127120000.

The authenticated poll must advertise 20261126000000 to the old client and the
current ten-file version to the bridge. The old client downloads from
/agent-update/. The bridge downloads from /agent-update-final/. The shipped
chat client again uses /agent-update/ for future releases. Firmware must
account for that when serving updates beyond chat.

The bridge keeps its own updater and supervisor active during the final worker
health trial. Before the health commit, cold recovery restores the previous
files and deletes chat files that were originally absent. After the durable
health commit, cold recovery finishes activating the signed ten-file release.
The complete signed image in the ordinary backup directory lets the final
updater finish recovery even after its own file has been replaced.

### An old client already suppresses the bridge

The old updater suppresses an offer at or below a recorded failed version.
The normal old verifier rejects the ten-file download before recording a
failure, but a previous health or stop failure can have left a higher marker.
An old client in that state cannot apply a fix inside the bridge. The
controller can also retain a client failure independently.

After correcting the original failure, use the trusted final chat bootstrap
for local recovery. Extract build/agent-release-chat-20261127120000/bootstrap.zip
and run update_codex_lan_agent.cmd as the installed user. This uses the saved
connection settings and installs the ten-file release directly. A recorded
failure at or below the installed version is ignored by the client on its
next poll. If a higher release is marked failed, investigate that release
before attempting it again. Preserve identity, configuration, and signing keys.
