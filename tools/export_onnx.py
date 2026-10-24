#!/usr/bin/env python3
"""Export a Real-ESRGAN SRVGGNetCompact checkpoint to ONNX for TensorRT/CUDA.

Example:
  python tools/export_onnx.py \
    --pth weights/realesr-general-x4v3.pth \
    --out assets/models/sr/onnx/realesr-general-x4v3.onnx \
    --scale 4 --num-conv 32
"""
import argparse
import os

import torch
import torch.nn as nn
import torch.nn.functional as F


class SRVGGNetCompact(nn.Module):
    def __init__(self, num_in_ch=3, num_out_ch=3, num_feat=64,
                 num_conv=32, upscale=4):
        super().__init__()
        body = [nn.Conv2d(num_in_ch, num_feat, 3, 1, 1),
                nn.PReLU(num_parameters=num_feat)]
        for _ in range(num_conv):
            body += [nn.Conv2d(num_feat, num_feat, 3, 1, 1),
                     nn.PReLU(num_parameters=num_feat)]
        body.append(nn.Conv2d(num_feat, num_out_ch * upscale * upscale, 3, 1, 1))
        self.body = nn.Sequential(*body)
        self.upscale = upscale

    def forward(self, x):
        feat = self.body(x)
        out = F.pixel_shuffle(feat, self.upscale)
        return out + F.interpolate(x, scale_factor=self.upscale, mode='nearest')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--pth', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--scale', type=int, default=4)
    p.add_argument('--num-conv', type=int, default=32)
    p.add_argument('--num-feat', type=int, default=64)
    p.add_argument('--opset', type=int, default=17)
    p.add_argument('--batch', type=int, default=1)
    args = p.parse_args()

    model = SRVGGNetCompact(num_feat=args.num_feat, num_conv=args.num_conv,
                            upscale=args.scale)
    raw = torch.load(args.pth, map_location='cpu', weights_only=True)
    sd = raw.get('params_ema', raw.get('params', raw))
    model.load_state_dict(sd, strict=True)
    model.eval()

    x = torch.randn(args.batch, 3, 64, 64)
    with torch.no_grad():
        y = model(x)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    torch.onnx.export(
        model, x, args.out,
        opset_version=args.opset,
        input_names=['input'], output_names=['output'],
        dynamic_axes={
            'input': {0: 'batch', 2: 'height', 3: 'width'},
            'output': {0: 'batch', 2: 'height', 3: 'width'},
        },
        do_constant_folding=True,
    )
    print(f'exported {args.out}')
    print(f'input={tuple(x.shape)} output={tuple(y.shape)} '
          f'scale={args.scale} opset={args.opset}')


if __name__ == '__main__':
    main()
