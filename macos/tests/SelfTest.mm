#import "tests/SelfTest.h"
#import "common/Support.h"
#include "core/CaptureClock.h"
#include "annotations/Annotation.h"
#include "annotations/ArrowGeometry.h"
#include "media/ExportQuality.h"
#include <iostream>
#include <stdexcept>
int SRCoreTests() {
    try {
        auto check=[](bool value,const char* name) { if (!value) throw std::runtime_error(name); std::cout<<"PASS "<<name<<'\n'; };
        sr::CaptureClock clock; check(!clock.Started(),"clock not started");
        clock.Start(100); check(clock.SampleTime(99)<0,"pre-origin audio rejected");
        clock.Pause(102); check(clock.SampleTime(103)<0 && clock.Elapsed(104)==2,"pause drops media and freezes duration");
        clock.Resume(105); check(clock.SampleTime(104)<0,"late pre-resume audio rejected");
        check(clock.SampleTime(106)==3 && clock.Elapsed(106)==3,"audio and video share pause offset");
        clock.Pause(107); clock.Resume(108); check(clock.SampleTime(109)==5,"multiple pauses do not accumulate drift");
        using namespace qrec::annotations;
        Document document; document.Reset({320,180},Time{2000});
        Mark mark=document.NewMark(Tool::Circle,Time{400}); mark.points={{80,80},{110,80}};
        check(mark.end-mark.start==Time{500},"default annotation is half a second");
        check(document.Put(mark),"insert circle");
        const Mark& circle=document.Current()->Marks()[0];
        check(HitTest(circle,{110,80},3) && !HitTest(circle,{80,80},3),"circle edge hit testing");
        check(Visible(circle,Time{400}) && !Visible(circle,Time{900}),"annotation half-open time range");
        mark=document.NewMark(Tool::Text,Time{1900}); mark.points={{10,10}}; mark.text=L"中文 🌍";
        check(document.Put(mark) && document.Current()->Marks().back().end==Time{2000},"text and end clamp");
        check(sr::Wide(sr::String(mark.text))==mark.text,"Unicode survives UTF-32 roundtrip");
        check(document.Undo() && document.Redo(),"undo and redo");
        auto size=qrec::media::ExportQuality::ComputeMp4Size(320,180,50);
        check(size.width==160 && size.height==90,"export resolution scales both dimensions");
        check(qrec::media::ExportQuality::Normalize(68)==70,"quality normalization");
        std::cout<<"All core tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
}
