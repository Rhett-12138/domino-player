#!/usr/bin/env python3
"""Export Real-ESRGAN checkpoints to ONNX for TensorRT/CUDA.

Examples:
  python tools/export_onnx.py --arch compact --pth weights/realesr-general-x4v3.pth \
    --out assets/models/sr/onnx/realesr-general-x4v3-fp16.onnx --fp16
  python tools/export_onnx.py --arch rrdb --pth weights/RealESRGAN_x4plus.pth \
    --out assets/models/sr/onnx/realesrgan-x4plus-fp16.onnx --fp16
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


class ResidualDenseBlock(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
        self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
        self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
        self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
        self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=False)

    def forward(self, x):
        x1 = self.lrelu(self.conv1(x))
        x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
        x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
        x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
        x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
        return x5 * 0.2 + x


class RRDB(nn.Module):
    def __init__(self, nf=64, gc=32):
        super().__init__()
        self.rdb1 = ResidualDenseBlock(nf, gc)
        self.rdb2 = ResidualDenseBlock(nf, gc)
        self.rdb3 = ResidualDenseBlock(nf, gc)

    def forward(self, x):
        return self.rdb3(self.rdb2(self.rdb1(x))) * 0.2 + x


class RRDBNet(nn.Module):
    def __init__(self, num_in_ch=3, num_out_ch=3, nf=64, nb=23, gc=32):
        super().__init__()
        self.conv_first = nn.Conv2d(num_in_ch, nf, 3, 1, 1)
        self.body = nn.Sequential(*[RRDB(nf, gc) for _ in range(nb)])
        self.conv_body = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up1 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_up2 = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_hr = nn.Conv2d(nf, nf, 3, 1, 1)
        self.conv_last = nn.Conv2d(nf, num_out_ch, 3, 1, 1)
        self.lrelu = nn.LeakyReLU(0.2, inplace=False)

    def forward(self, x):
        feat = self.conv_first(x)
        feat = feat + self.conv_body(self.body(feat))
        feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode='nearest')))
        feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode='nearest')))
        return self.conv_last(self.lrelu(self.conv_hr(feat)))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--arch', choices=['compact', 'rrdb'], default='compact')
    p.add_argument('--pth', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--scale', type=int, default=4)
    p.add_argument('--opset', type=int, default=17)
    p.add_argument('--batch', type=int, default=1)
    p.add_argument('--num-conv', type=int, default=32)
    p.add_argument('--num-feat', type=int, default=64)
    p.add_argument('--num-block', type=int, default=23)
    p.add_argument('--grow-ch', type=int, default=32)
    p.add_argument('--fp16', action='store_true')
    args = p.parse_args()

    if args.arch == 'compact':
        model = SRVGGNetCompact(num_feat=args.num_feat, num_conv=args.num_conv,
                                upscale=args.scale)
    else:
        model = RRDBNet(nf=args.num_feat, nb=args.num_block, gc=args.grow_ch)
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
    if args.fp16:
        import onnx
        from onnxconverter_common import float16
        m = onnx.load(args.out)
        m = float16.convert_float_to_float16(m, keep_io_types=False,
                                            disable_shape_infer=False)
        onnx.checker.check_model(m)
        onnx.save(m, args.out)
    print(f'exported {args.out}')
    print(f'input={tuple(x.shape)} output={tuple(y.shape)} '
          f'scale={args.scale} opset={args.opset} fp16={args.fp16}')


if __name__ == '__main__':
    main()
