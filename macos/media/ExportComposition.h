#pragma once
// Build a trimmed/scaled composition and a GPU-compatible, cached annotation renderer.
#import "media/ExportJob.h"
AVMutableComposition* SRComposition(SRExportRequest* request,NSError** error);
AVMutableVideoComposition* SRVideoComposition(AVAsset* asset,SRExportRequest* request,BOOL gif);
BOOL SRExportGIF(AVAsset* asset,SRExportRequest* request,NSURL* output,SRExportJob* job,NSError** error);
