#pragma once
// Pure-model tests and synthetic-media regression: no live screen recording or user input.
#import <Foundation/Foundation.h>
int SRCoreTests();
int SRMediaTests(NSURL* directory);
@class SREditorController;
void SRUITests(SREditorController* editor);
