#10800
Pageboy spawn~
0 n 100
~
if (!%instance.location%)
  halt
end
mgoto %instance.location%
eval room %self.room%
if %room.exit_dir%
  %room.exit_dir%
end
~
#10801
Give rejection~
0 j 100
~
%send% %actor% You can't give items to %self.name%. Try "quest finish <name>" instead.
return 0
~
#10802
Pageboy shout~
0 ab 1
~
if %random.3% == 3
  %regionecho% %self.room% 20 %self.name% shouts, 'Learn the Empire skill at the (^^) Royal Planning Office!'
end
~
#10825
Crier spawn~
0 n 100
~
if (!%instance.location%)
  halt
end
mgoto %instance.location%
eval room %self.room%
if %room.exit_dir%
  %room.exit_dir%
end
~
#10826
Give rejection~
0 j 100
~
%send% %actor% You can't give items to %self.name%. Try "quest finish <name>" instead.
return 0
~
#10827
Crier Shout~
0 ab 1
~
if %random.3% == 3
  %regionecho% %self.room% 20 %self.name% shouts, 'Learn the Trade skill at the &y/()\&0 Museum of Early Man!'
end
~
#10828
Curator environment~
0 b 5
~
switch %random.4%
  case 1
    say Maybe I should hire some skeletons to stand around and chat with visitors.
  break
  case 2
    %echo% %self.name% sprinkles some authentic-looking dust on the shelves.
  break
  case 3
    %echo% %self.name% rubs a piece of leather in the dirt, then heaps it on a shelf.
  break
  case 4
    %echo% %self.name% jingles a jar labeled 'Admissions', but it sounds empty.
  break
done
~
#10829
Cave Phase 2 Linker~
2 n 100
~
if !%instance.id%
  halt
end
eval entr %instance.location%
eval dir %entr.exit_dir%
eval outside %%entr.%dir%(vnum)%%
%door% %room% %dir% room %outside%
detach 10829 %room.id%
~
#10830
Cave Phase 2 Teleporter~
2 g 100
~
if (%actor.is_pc% && %actor.completed_quest(10835)%)
  %teleport% %actor% i10826
  halt
end
eval boss %actor.master%
if (%actor.is_npc% && %boss%)
  %teleport% %actor% %boss.room%
end
~
#10851
Detect Ritual of Burdens~
2 p 100
~
if (%ability% != 163 || !%actor.on_quest(10851)%)
  halt
end
eval invsize %actor.maxcarrying%
wait 5 s
* Attempt to detect that they cast the ritual
if %actor.maxcarrying% > %invsize% || %invsize% > 25
  %quest% %actor% trigger 10851
  %send% %actor% You have completed the ritual for your quest.
end
~
#10852
Give Scythe~
2 u 100
~
%load% obj 10856 %actor%
~
#10853
Give Skinning Knife~
2 u 100
~
%load% obj 10853 %actor%
~
#10855
Detect Sneak~
0 h 100
~
if (%actor.aff_flagged(SNEAK)% && %actor.on_quest(10855)%)
  %quest% %actor% trigger 10855
  wait 1
  %send% %actor% You have successfully sneaked up on %self.name%!
end
~
#10856
Detect Harvest~
1 c 3
harvest~
eval room %actor.room%
if %room.crop%
  %quest% %actor% trigger 10856
end
return 0
~
$