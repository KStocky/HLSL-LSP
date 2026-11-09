using System;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;
using Microsoft.VisualStudio.Text;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslDocumentWindowMappingTests
{
    [Fact]
    public void FirstShowRemapsTheCurrentBuffer()
    {
        var buffer = (ITextBuffer)new BufferProxy().GetTransparentProxy();
        ITextBuffer mapped = null;
        uint requestedCookie = 0;

        HlslLspActivator.MapFirstShownDocument(
            42,
            1,
            cookie =>
            {
                requestedCookie = cookie;
                return buffer;
            },
            candidate => mapped = candidate);

        Assert.Equal(42u, requestedCookie);
        Assert.Same(buffer, mapped);
    }

    [Fact]
    public void SubsequentShowsDoNotRemapDemotedBuffers()
    {
        HlslLspActivator.MapFirstShownDocument(
            42,
            0,
            _ => throw new InvalidOperationException("The buffer should not be queried."),
            _ => throw new InvalidOperationException("The buffer should not be remapped."));
    }

    [Fact]
    public void FirstShowWithoutATextBufferDoesNotApplyAContentType()
    {
        HlslLspActivator.MapFirstShownDocument(
            42,
            1,
            _ => null,
            _ => throw new InvalidOperationException("There is no text buffer to remap."));
    }

    private sealed class BufferProxy : RealProxy
    {
        internal BufferProxy() : base(typeof(ITextBuffer))
        {
        }

        public override IMessage Invoke(IMessage message)
            => throw new NotSupportedException();
    }
}
