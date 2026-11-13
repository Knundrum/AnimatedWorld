# AnimatedWorld (CommonLibF4RD)

Port of AnimatedWorld from the in-tree CommonLibF4 fork to
[CommonLibF4RD](https://github.com/Zzyxz/CommonLibF4RD).

Behaviour on Fallout 4 1.10.163 (OG) is unchanged apart from the bug fixes listed
below. **AE 1.11.240 is supported**: every address resolves, all six hook sites
are located by `REL::AUTO_CALLSITE`, and activate, equip, consume, add-item and
ground pickup are confirmed in game.

NG 1.10.984 is untested. Two of its interior offsets are deliberately left unset
so those sites fail closed there rather than writing a branch into an unverified
address — see **Runtime support**.
