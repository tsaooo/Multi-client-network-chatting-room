# Multi-client network chatting room

Using single process concurrent server architechure to implement a remote chatting room with remote pipe function.

For the functionality detail, plz refer to the spec.


```shell=
% *** User ’(no name)’ entered from 140.113.215.64:1302. *** 
who
<ID> <nickname> <IP:port>           <indicate me>
1     Jamie     140.113.215.62:1201 <- me
2     Roger     140.113.215.63:1013
3     (no name) 140.113.215.64:1302
% yell Who knows how to make egg fried rice? help me plz!
*** Jamie yelled ***: Who knows how to make egg fried rice? help me plz!
% *** (no name) yelled ***: Sorry, I don’t know. :-( 
*** Roger yelled ***: HAIYAAAAAAA !!!  
% tell 2 Plz help me, my friends!
% *** Roger told you ***: Yeah! Let me show you the recipe
*** Roger just piped ’cat EggFriedRice.txt >1’ to Jamie
*** Roger told you ***: You can use ’cat <2’ to show it!
cat <5
*** Error: user #5 does not exist yet. ***
% cat <2
*** Jamie (#1) just received from Roger (#2) by ’cat <2’ ***
Ask Uncle Gordon
Season with MSG !!
```
