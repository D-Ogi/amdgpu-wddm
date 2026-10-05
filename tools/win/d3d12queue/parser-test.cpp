// SPDX-License-Identifier: MIT
#include "interactive.h"
#include <cassert>
#include <cstdio>
int main(){
 using namespace interactive;
 for(auto verb:{Verb::CreateDevice,Verb::CreateQueue,Verb::Copy,Verb::Status,Verb::Exit,Verb::Abort}){
  auto parsed=parse(std::string("1 ")+name(verb)+"\n",1);assert(parsed.sequence==1 && parsed.verb==verb);
 }
 assert(parse("64 status\r\n",64).verb==Verb::Status);
 for(const auto& bad:{"", "0 status", "65 status", "1 status extra", "1  status", "1 STATUS", "1 copy\n1 exit", "1 status\rX", "-1 exit", "9999999999999999 status"})assert(parse(bad,1).verb==Verb::Invalid);
 assert(parse("2 copy",1).verb==Verb::Invalid);
 assert(parse(std::string("1 status\0extra",14),1).verb==Verb::Invalid);
 assert(parse(std::string(100,'1'),1).verb==Verb::Invalid);
 assert(seconds("70")==70 && seconds("150")==150 && !seconds("151") && !seconds("0") && !seconds("-1") && !seconds("9999999999999999999") && !seconds("70x"));
 std::puts("PASS interactive command parser: six commands, sequence bounds, malformed/trailing/NUL input and deadline limits; no GPU calls");
}
