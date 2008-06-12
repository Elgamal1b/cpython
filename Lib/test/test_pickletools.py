import pickle
import pickletools
from test import support
from test.pickletester import AbstractPickleTests
from test.pickletester import AbstractPickleModuleTests

class OptimizedPickleTests(AbstractPickleTests, AbstractPickleModuleTests):

    def dumps(self, arg, proto=None):
        return pickletools.optimize(pickle.dumps(arg, proto))

    def loads(self, buf):
        return pickle.loads(buf)

    module = pickle
    error = KeyError

def test_main():
    support.run_unittest(OptimizedPickleTests)
    support.run_doctest(pickletools)


if __name__ == "__main__":
    test_main()
